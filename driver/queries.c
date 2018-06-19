/*
 * Copyright Elasticsearch B.V. and/or licensed to Elasticsearch B.V. under one
 * or more contributor license agreements. Licensed under the Elastic License;
 * you may not use this file except in compliance with the Elastic License.
 */

#include <inttypes.h>
#include <stdlib.h>
#include <windows.h> /* WideCharToMultiByte() */
#include <float.h>
#include <math.h>

#include "ujdecode.h"
#include "timestamp.h"

#include "queries.h"
#include "log.h"
#include "connect.h"
#include "info.h"

/* key names used in Elastic/SQL REST/JSON answers */
#define JSON_ANSWER_COLUMNS		"columns"
#define JSON_ANSWER_ROWS		"rows"
#define JSON_ANSWER_CURSOR		"cursor"
#define JSON_ANSWER_STATUS		"status"
#define JSON_ANSWER_ERROR		"error"
#define JSON_ANSWER_ERR_TYPE	"type"
#define JSON_ANSWER_ERR_REASON	"reason"
#define JSON_ANSWER_COL_NAME	"name"
#define JSON_ANSWER_COL_TYPE	"type"


#define MSG_INV_SRV_ANS		"Invalid server answer"

#define TM_TO_TIMESTAMP_STRUCT(_tmp/*src*/, _tsp/*dst*/) \
	do { \
		(_tsp)->year = (_tmp)->tm_year + 1900; \
		(_tsp)->month = (_tmp)->tm_mon + 1; \
		(_tsp)->day = (_tmp)->tm_mday; \
		(_tsp)->hour = (_tmp)->tm_hour; \
		(_tsp)->minute = (_tmp)->tm_min; \
		(_tsp)->second = (_tmp)->tm_sec; \
	} while (0)

/* For fixed size (destination) types, the target buffer can't be NULL. */
#define REJECT_IF_NULL_DEST_BUFF(_s/*tatement*/, _p/*ointer*/) \
	do { \
		if (! _p) { \
			ERRH(_s, "destination buffer can't be NULL."); \
			RET_HDIAGS(stmt, SQL_STATE_HY009); \
		} \
	} while (0)
#define REJECT_AS_OOR(_stmt, _val, _fix_val, _target) /* Out Of Range */ \
	do { \
		if (_fix_val) { \
			ERRH(_stmt, "can't convert value %lld to %s: out of range", \
				_val, STR(_target)); \
		} else { \
			ERRH(_stmt, "can't convert value %f to %s: out of range", \
				_val, STR(_target)); \
		} \
		RET_HDIAGS(_stmt, SQL_STATE_22003); \
	} while (0)

/* TODO: this is inefficient: add directly into ujson4c lib (as .size of
 * ArrayItem struct, inc'd in arrayAddItem()) or local utils file. */
static size_t UJArraySize(UJObject obj)
{
	UJObject _u;
	size_t size = 0;
	void *iter = UJBeginArray(obj);
	if (iter) {
		while (UJIterArray(&iter, &_u)) {
			size ++;
		}
	}
	return size;
}


void clear_resultset(esodbc_stmt_st *stmt)
{
	DBGH(stmt, "clearing result set; vrows=%zd, nrows=%zd, frows=%zd.",
		stmt->rset.vrows, stmt->rset.nrows, stmt->rset.frows);
	if (stmt->rset.buff) {
		free(stmt->rset.buff);
	}
	if (stmt->rset.state) {
		UJFree(stmt->rset.state);
	}
	memset(&stmt->rset, 0, sizeof(stmt->rset));
}

/* Set the desriptor fields associated with "size". This step is needed since
 * the application could read the descriptors - like .length - individually,
 * rather than through functions that make use of get_col_size() (where we
 * could just read the es_type directly). */
static void set_col_size(esodbc_rec_st *rec)
{
	assert(rec->desc->type == DESC_TYPE_IRD);

	switch (rec->meta_type) {
		case METATYPE_UNKNOWN:
			/* SYS TYPES call */
			break;
		case METATYPE_EXACT_NUMERIC:
		case METATYPE_FLOAT_NUMERIC:
			/* ignore, the .precision field is not used in IRDs, its value is
			 * always read from es_type.column_size directly */
			break;

		/*
		 * https://docs.microsoft.com/en-us/sql/odbc/reference/appendixes/column-size : */
		case METATYPE_STRING:
		/* "The defined or maximum column size in characters of the
		 * column" */
		/* no break */
		case METATYPE_BIN:
		/* "The defined or maximum length in bytes of the column " */
		/* no break */
		case METATYPE_DATETIME:
			/* "number of characters in the character representation" */
			rec->length = rec->es_type->column_size;
			break;

		default:
			BUGH(rec->desc, "unsupported data c-type: %d.", rec->concise_type);
	}
}

static SQLRETURN attach_columns(esodbc_stmt_st *stmt, UJObject columns)
{
	esodbc_desc_st *ird;
	esodbc_dbc_st *dbc;
	esodbc_rec_st *rec;
	SQLRETURN ret;
	SQLSMALLINT recno;
	void *iter;
	UJObject col_o, name_o, type_o;
	wstr_st col_type;
	size_t ncols, i;
	const wchar_t *keys[] = {
		MK_WPTR(JSON_ANSWER_COL_NAME),
		MK_WPTR(JSON_ANSWER_COL_TYPE)
	};

	ird = stmt->ird;
	dbc = stmt->hdr.dbc;

	ncols = UJArraySize(columns);
	DBGH(stmt, "columns received: %zd.", ncols);
	ret = update_rec_count(ird, (SQLSMALLINT)ncols);
	if (! SQL_SUCCEEDED(ret)) {
		ERRH(stmt, "failed to set IRD's record count to %d.", ncols);
		HDIAG_COPY(ird, stmt);
		return ret;
	}

	iter = UJBeginArray(columns);
	if (! iter) {
		ERRH(stmt, "failed to obtain array iterator: %s.",
			UJGetError(stmt->rset.state));
		RET_HDIAG(stmt, SQL_STATE_HY000, MSG_INV_SRV_ANS, 0);
	}
	recno = 0;
	while (UJIterArray(&iter, &col_o)) {
		if (UJObjectUnpack(col_o, 2, "SS", keys, &name_o, &type_o) < 2) {
			ERRH(stmt, "failed to decode JSON column: %s.",
				UJGetError(stmt->rset.state));
			RET_HDIAG(stmt, SQL_STATE_HY000, MSG_INV_SRV_ANS, 0);
		}
		rec = &ird->recs[recno]; // +recno

		ASSERT_INTEGER_TYPES_EQUAL(wchar_t, SQLWCHAR);
		rec->name.str = (SQLWCHAR *)UJReadString(name_o, &rec->name.cnt);
		if (! rec->name.str) {
			rec->name = MK_WSTR("");
		}

		col_type.str = (SQLWCHAR *)UJReadString(type_o, &col_type.cnt);

		assert(! rec->es_type);
		/* lookup the DBC-cashed ES type */
		for (i = 0; i < dbc->no_types; i ++) {
			if (EQ_CASE_WSTR(&dbc->es_types[i].type_name, &col_type)) {
				rec->es_type = &dbc->es_types[i];
				break;
			}
		}
		if (rec->es_type) {
			/* copy fileds pre-calculated at DB connect time */
			rec->concise_type = rec->es_type->data_type;
			rec->type = rec->es_type->sql_data_type;
			rec->datetime_interval_code = rec->es_type->sql_datetime_sub;
			rec->meta_type = rec->es_type->meta_type;
		} else if (! dbc->no_types) {
			/* the connection doesn't have yet the types cached (this is the
			 * caching call) and don't have access to the data itself either,
			 * just the column names & type names => set unknowns.  */
			rec->concise_type = SQL_UNKNOWN_TYPE;
			rec->type = SQL_UNKNOWN_TYPE;
			rec->datetime_interval_code = 0;
			rec->meta_type = METATYPE_UNKNOWN;
		} else {
			ERRH(stmt, "type lookup failed for `" LWPDL "`.",LWSTR(&col_type));
			RET_HDIAG(stmt, SQL_STATE_HY000, MSG_INV_SRV_ANS, 0);
		}

		set_col_size(rec);

		/* TODO: set remaining of settable fields (base table etc.) */

		/* "If a base column name does not exist (as in the case of columns
		 * that are expressions), then this variable contains an empty
		 * string." */
		rec->base_column_name = MK_WSTR("");
		/* "If a column does not have a label, the column name is returned. If
		 * the column is unlabeled and unnamed, an empty string is ret" */
		rec->label = rec->name.cnt ? rec->name : MK_WSTR("");

		assert(rec->name.str && rec->label.str);
		rec->unnamed = (rec->name.cnt || rec->label.cnt) ?
			SQL_NAMED : SQL_UNNAMED;

#ifndef NDEBUG
		//dump_record(rec);
#endif /* NDEBUG */

		DBGH(stmt, "column #%d: name=`" LWPDL "`, type=%d (`" LWPDL "`).",
			recno, LWSTR(&rec->name), rec->concise_type, LWSTR(&col_type));
		recno ++;
	}

	/* new columsn attached, need to check compatiblity */
	stmt->sql2c_conversion = CONVERSION_UNCHECKED;

	return SQL_SUCCESS;
}


/*
 * Processes a received answer:
 * - takes a dynamic buffer, buff, of length blen. Will handle the buff memory
 * even if the call fails.
 * - parses it, preparing iterators for SQLFetch()'ing.
 */
SQLRETURN TEST_API attach_answer(esodbc_stmt_st *stmt, char *buff, size_t blen)
{
	int unpacked;
	UJObject obj, columns, rows, cursor;
	const wchar_t *wcurs;
	size_t eccnt;
	const wchar_t *keys[] = {
		MK_WPTR(JSON_ANSWER_COLUMNS),
		MK_WPTR(JSON_ANSWER_ROWS),
		MK_WPTR(JSON_ANSWER_CURSOR)
	};

	/* clear any previous result set */
	if (STMT_HAS_RESULTSET(stmt)) {
		clear_resultset(stmt);
	}

	/* the statement takes ownership of mem obj */
	stmt->rset.buff = buff;
	stmt->rset.blen = blen;
	DBGH(stmt, "attaching answer [%zd]`" LCPDL "`.", blen, blen, buff);

	/* parse the entire JSON answer */
	obj = UJDecode(buff, blen, NULL, &stmt->rset.state);
	if (! obj) {
		ERRH(stmt, "failed to decode JSON answer (`%.*s`): %s.", blen, buff,
			stmt->rset.state ? UJGetError(stmt->rset.state) : "<none>");
		RET_HDIAG(stmt, SQL_STATE_HY000, MSG_INV_SRV_ANS, 0);
	}
	columns = rows = cursor = NULL;
	/* extract the columns and rows objects */
	unpacked = UJObjectUnpack(obj, 3, "AAS", keys, &columns, &rows, &cursor);
	if (unpacked < /* 'rows' must always be present */1) {
		ERRH(stmt, "failed to unpack JSON answer (`%.*s`): %s.",
			blen, buff, UJGetError(stmt->rset.state));
		RET_HDIAG(stmt, SQL_STATE_HY000, MSG_INV_SRV_ANS, 0);
	}

	/*
	 * set the internal cursor (UJSON4C array iterator)
	 */
	if (! rows) {
		ERRH(stmt, "no rows JSON object received in answer: `%.*s`[%zd].",
			blen, buff, blen);
		RET_HDIAG(stmt, SQL_STATE_HY000, MSG_INV_SRV_ANS, 0);
	}
	stmt->rset.rows_iter = UJBeginArray(rows);
	if (! stmt->rset.rows_iter) {
#if 0 /* UJSON4C will return NULL above, for empty array (meh!) */
		ERRH(stmt, "failed to get iterrator on received rows: %s.",
			UJGetError(stmt->rset.state));
		RET_HDIAGS(stmt, SQL_STATE_HY000);
#else /*0*/
		DBGH(stmt, "received empty resultset array: forcing nodata.");
		STMT_FORCE_NODATA(stmt);
		stmt->rset.nrows = 0;
#endif /*0*/
	} else {
		stmt->rset.nrows = (size_t)UJArraySize(rows);
	}
	DBGH(stmt, "rows received in result set: %zd.", stmt->rset.nrows);

	/*
	 * copy Elastic's cursor (if there's one)
	 */
	if (cursor) {
		wcurs = UJReadString(cursor, &eccnt);
		if (eccnt) {
			/* this can happen automatically if hitting scroller size */
			if (! stmt->hdr.dbc->fetch.max) {
				INFOH(stmt, "no fetch size defined, but cursor returned.");
			}
			if (stmt->rset.ecurs)
				DBGH(stmt, "replacing old cursor `" LWPDL "`.",
					stmt->rset.eccnt, stmt->rset.ecurs);
			/* store new cursor vals */
			stmt->rset.ecurs = wcurs;
			stmt->rset.eccnt = eccnt;
			DBGH(stmt, "new elastic cursor: `" LWPDL "`[%zd].",
				stmt->rset.eccnt, stmt->rset.ecurs, stmt->rset.eccnt);
		} else {
			WARNH(stmt, "empty cursor found in the answer.");
		}
	} else {
		/* should have been cleared by now */
		assert(! stmt->rset.eccnt);
	}

	/*
	 * process the sent columns, if any.
	 */
	if (columns) {
		if (0 < stmt->ird->count) {
			ERRH(stmt, "%d columns already attached.", stmt->ird->count);
			RET_HDIAG(stmt, SQL_STATE_HY000, MSG_INV_SRV_ANS, 0);
		}
		return attach_columns(stmt, columns);
	} else {
		/* no cols available in this answer: check if already received */
		if (stmt->ird->count <= 0) {
			ERRH(stmt, "no columns available in result set; answer: "
				"`%.*s`[%zd].", blen, buff, blen);
			RET_HDIAG(stmt, SQL_STATE_HY000, MSG_INV_SRV_ANS, 0);
		}
	}

	return SQL_SUCCESS;
}

/*
 * Parse an error and push it as statement diagnostic.
 */
SQLRETURN TEST_API attach_error(esodbc_stmt_st *stmt, char *buff, size_t blen)
{
	UJObject obj, o_status, o_error, o_type, o_reason;
	const wchar_t *wtype, *wreason;
	size_t tlen, rlen, left;
	wchar_t wbuf[sizeof(((esodbc_diag_st *)NULL)->text) /
								sizeof(*((esodbc_diag_st *)NULL)->text)];
	size_t wbuflen = sizeof(wbuf)/sizeof(*wbuf);
	int n;
	void *state = NULL;
	const wchar_t *outer_keys[] = {
		MK_WPTR(JSON_ANSWER_ERROR),
		MK_WPTR(JSON_ANSWER_STATUS)
	};
	const wchar_t *err_keys[] = {
		MK_WPTR(JSON_ANSWER_ERR_TYPE),
		MK_WPTR(JSON_ANSWER_ERR_REASON)
	};

	INFOH(stmt, "REST request failed with `%.*s` (%zd).", blen, buff, blen);

	/* parse the entire JSON answer */
	obj = UJDecode(buff, blen, NULL, &state);
	if (! obj) {
		ERRH(stmt, "failed to decode JSON answer (`%.*s`): %s.",
			blen, buff, state ? UJGetError(state) : "<none>");
		SET_HDIAG(stmt, SQL_STATE_HY000, MSG_INV_SRV_ANS, 0);
		goto end;
	}
	/* extract the status and error object */
	if (UJObjectUnpack(obj, 2, "ON", outer_keys, &o_error, &o_status) < 2) {
		ERRH(stmt, "failed to unpack JSON answer (`%.*s`): %s.",
			blen, buff, UJGetError(state));
		SET_HDIAG(stmt, SQL_STATE_HY000, MSG_INV_SRV_ANS, 0);
		goto end;
	}
	/* unpack error object */
	if (UJObjectUnpack(o_error, 2, "SS", err_keys, &o_type, &o_reason) < 2) {
		ERRH(stmt, "failed to unpack error object (`%.*s`): %s.",
			blen, buff, UJGetError(state));
		SET_HDIAG(stmt, SQL_STATE_HY000, MSG_INV_SRV_ANS, 0);
		goto end;
	}

	wtype = UJReadString(o_type, &tlen);
	wreason = UJReadString(o_reason, &rlen);
	/* these return empty string in case of mismatch */
	assert(wtype && wreason);
	DBGH(stmt, "server failures: type: [%zd] `" LWPDL "`, reason: [%zd] `"
		LWPDL "`, status: %d.", tlen, tlen, wtype, rlen, rlen, wreason,
		UJNumericInt(o_status));

	/* swprintf will fail if formated string would overrun the buffer size (as
	 * opposed to write up to its limit) => find out the limit first.*/
	n = swprintf(NULL, 0, MK_WPTR("%.*s: %.*s"), (int)tlen, wtype, (int)rlen,
			wreason);
	if (0 < n) {
		wbuflen -= /* ": " */2 + /*\0*/1;
		tlen = wbuflen < tlen ? wbuflen : tlen;
		left = wbuflen - tlen;
		rlen = left < rlen ? left : rlen;
		wbuflen += /* ": " */2 + /*\0*/1;
		/* swprintf will add the 0-term (or fail, if it can't) */
		n = swprintf(wbuf, wbuflen, MK_WPTR("%.*s: %.*s"), (int)tlen, wtype,
				(int)rlen, wreason);
	}
	if (n < 0) {
		ERRNH(stmt, "failed to print error message from server.");
		assert(sizeof(MSG_INV_SRV_ANS) < sizeof(wbuf));
		memcpy(wbuf, MK_WPTR(MSG_INV_SRV_ANS),
			sizeof(MSG_INV_SRV_ANS)*sizeof(SQLWCHAR));
	}

	post_diagnostic(&stmt->hdr.diag, SQL_STATE_HY000, wbuf,
		UJNumericInt(o_status));

end:
	if (state) {
		UJFree(state);
	}
	if (buff) {
		free(buff);
	}

	RET_STATE(stmt->hdr.diag.state);
}

/*
 * Attach an SQL query to the statment: malloc, convert, copy.
 */
SQLRETURN attach_sql(esodbc_stmt_st *stmt,
	const SQLWCHAR *sql, /* SQL text statement */
	size_t sqlcnt /* count of chars of 'sql' */)
{
	char *u8;
	int len;

	DBGH(stmt, "attaching SQL `" LWPDL "` (%zd).", sqlcnt, sql, sqlcnt);
#if 0 // FIXME
	if (wcslen(sql) < 1256) {
		if (wcsstr(sql, L"FROM test_emp")) {
			sql = L"SELECT emp_no, first_name, last_name, birth_date, 2+3 AS foo FROM test_emp";
			sqlcnt = wcslen(sql);
			DBGH(stmt, "RE-attaching SQL `" LWPDL "` (%zd).", sqlcnt,
				sql, sqlcnt);
		}
	}
#endif

	assert(! stmt->u8sql);

	len = WCS2U8(sql, (int)sqlcnt, NULL, 0);
	if (len <= 0) {
		ERRNH(stmt, "failed to UCS2/UTF8 convert SQL `" LWPDL "` (%zd).",
			sqlcnt, sql, sqlcnt);
		RET_HDIAG(stmt, SQL_STATE_HY000, "UCS2/UTF8 conversion failure", 0);
	}
	DBGH(stmt, "wide char SQL `" LWPDL "`[%zd] converts to UTF8 on %d "
		"octets.", sqlcnt, sql, sqlcnt, len);

	u8 = malloc(len);
	if (! u8) {
		ERRNH(stmt, "failed to alloc %dB.", len);
		RET_HDIAGS(stmt, SQL_STATE_HY001);
	}

	len = WCS2U8(sql, (int)sqlcnt, u8, len);
	if (len <= 0) { /* can it happen? it's just succeded above */
		ERRNH(stmt, "failed to UCS2/UTF8 convert SQL `" LWPDL "` (%zd).",
			sqlcnt, sql, sqlcnt);
		free(u8);
		RET_HDIAG(stmt, SQL_STATE_HY000, "UCS2/UTF8 conversion failure(2)", 0);
	}

	stmt->u8sql = u8;
	stmt->sqllen = (size_t)len;

	DBGH(stmt, "attached SQL `%.*s` (%zd).", len, u8, len);

	return SQL_SUCCESS;
}

/*
 * Detach the existing query (if any) from the statement.
 */
void detach_sql(esodbc_stmt_st *stmt)
{
	if (! stmt->u8sql) {
		return;
	}
	free(stmt->u8sql);
	stmt->u8sql = NULL;
	stmt->sqllen = 0;
}


/*
 * "An application can unbind the data buffer for a column but still have a
 * length/indicator buffer bound for the column, if the TargetValuePtr
 * argument in the call to SQLBindCol is a null pointer but the
 * StrLen_or_IndPtr argument is a valid value."
 *
 * "When the driver returns fixed-length data, such as an integer or a date
 * structure, the driver ignores BufferLength and assumes the buffer is large
 * enough to hold the data." BUT:
 * "This is an error if the data returned by the driver is NULL but is common
 * when retrieving fixed-length, non-nullable data, because neither a length
 * nor an indicator value is needed."
 *
 * "The binding remains in effect until it is replaced by a new binding, the
 * column is unbound, or the statement is freed."
 *
 * "If ColumnNumber refers to an unbound column, SQLBindCol still returns
 * SQL_SUCCESS."
 *
 * "Call SQLBindCol to specify a new binding for a column that is already
 * bound. The driver overwrites the old binding with the new one."
 *
 * "Binding Offsets: the same offset is added to each address in each binding"
 *
 * "https://docs.microsoft.com/en-us/sql/odbc/reference/develop-app/buffers":
 * SQL_LEN_BINARY_ATTR, SQL_NTS, SQL_IS_POINTER/_INTEGER/etc.
 *
 * " The application sets the SQL_BIND_BY_COLUMN statement attribute to
 * specify whether it is using column-wise or row-wise binding"
 */
SQLRETURN EsSQLBindCol(
	SQLHSTMT StatementHandle,
	SQLUSMALLINT ColumnNumber,
	SQLSMALLINT TargetType, /* identifier of the C data type */
	_Inout_updates_opt_(_Inexpressible_(BufferLength))
	SQLPOINTER TargetValue,
	SQLLEN BufferLength,
	_Inout_opt_ SQLLEN *StrLen_or_Ind)
{
	SQLRETURN ret;
	esodbc_stmt_st *stmt = STMH(StatementHandle);
	esodbc_desc_st *ard = stmt->ard;

	if (BufferLength < 0) {
		ERRH(stmt, "invalid negative BufferLength: %d.", BufferLength);
		RET_HDIAGS(STMH(StatementHandle), SQL_STATE_HY090);
	}

	if ((STMH(StatementHandle)->bookmarks != SQL_UB_OFF) || (! ColumnNumber)) {
		/* "The statement attribute SQL_ATTR_USE_BOOKMARKS should always be
		 * set before binding a column to column 0. This is not required but
		 * is strongly recommended." */
		//RET_HDIAGS(STMH(StatementHandle), SQL_STATE_IM001);
		/* TODO: implement bookmarks? */
		FIXME;
	}

	/* "if the value in the ColumnNumber argument exceeds the value of
	 * SQL_DESC_COUNT, calls SQLSetDescField to increase the value of
	 * SQL_DESC_COUNT to ColumnNumber." */
	if (ard->count < ColumnNumber) {
		ret = EsSQLSetDescFieldW(ard, NO_REC_NR, SQL_DESC_COUNT,
				(SQLPOINTER)(uintptr_t)ColumnNumber, SQL_IS_SMALLINT);
		if (ret != SQL_SUCCESS) {
			goto copy_ret;
		}
	}

	/* set concise type (or verbose for datetime/interval types) */
	ret = EsSQLSetDescFieldW(ard, ColumnNumber, SQL_DESC_CONCISE_TYPE,
			(SQLPOINTER)(intptr_t)TargetType, SQL_IS_SMALLINT);
	if (ret != SQL_SUCCESS) {
		goto copy_ret;
	}

	// TODO: "Sets one or more of SQL_DESC_LENGTH, SQL_DESC_PRECISION,
	// SQL_DESC_SCALE, and SQL_DESC_DATETIME_INTERVAL_PRECISION, as
	// appropriate for TargetType."
	// TODO: Cautions Regarding SQL_DEFAULT

	/* "Sets the SQL_DESC_OCTET_LENGTH field to the value of BufferLength." */
	ret = EsSQLSetDescFieldW(ard, ColumnNumber, SQL_DESC_OCTET_LENGTH,
			(SQLPOINTER)(intptr_t)BufferLength, SQL_IS_INTEGER);
	if (ret != SQL_SUCCESS) {
		goto copy_ret;
	}

	/* Sets the SQL_DESC_INDICATOR_PTR field to the value of StrLen_or_Ind" */
	ret = EsSQLSetDescFieldW(ard, ColumnNumber, SQL_DESC_INDICATOR_PTR,
			StrLen_or_Ind,
			SQL_LEN_BINARY_ATTR((SQLINTEGER)sizeof(StrLen_or_Ind)));
	if (ret != SQL_SUCCESS) {
		goto copy_ret;
	}

	/* "Sets the SQL_DESC_OCTET_LENGTH_PTR field to the value of
	 * StrLen_or_Ind." */
	ret = EsSQLSetDescFieldW(ard, ColumnNumber, SQL_DESC_OCTET_LENGTH_PTR,
			StrLen_or_Ind,
			SQL_LEN_BINARY_ATTR((SQLINTEGER)sizeof(StrLen_or_Ind)));
	if (ret != SQL_SUCCESS) {
		goto copy_ret;
	}

	/* "Sets the SQL_DESC_DATA_PTR field to the value of TargetValue."
	 * Note: needs to be last set field, as setting other fields unbind. */
	ret = EsSQLSetDescFieldW(ard, ColumnNumber, SQL_DESC_DATA_PTR,
			TargetValue, SQL_IS_POINTER);
	if (ret != SQL_SUCCESS) {
		goto copy_ret;
	}

	/* every binding resets conversion flag */
	stmt->sql2c_conversion = CONVERSION_UNCHECKED;

	return SQL_SUCCESS;

copy_ret:
	/* copy error at top handle level, where it's going to be inquired from */
	HDIAG_COPY(ard, stmt);
	return ret;
}

/*
 * field: SQL_DESC_: DATA_PTR / INDICATOR_PTR / OCTET_LENGTH_PTR
 * pos: position in array/row_set (not result_set)
 */
static void *deferred_address(SQLSMALLINT field_id, size_t pos,
	esodbc_rec_st *rec)
{
	size_t elem_size;
	SQLLEN offt;
	void *base;
	esodbc_desc_st *desc = rec->desc;

#define ROW_OFFSETS \
	do { \
		elem_size = desc->bind_type; \
		offt = desc->bind_offset_ptr ? *(desc->bind_offset_ptr) : 0; \
	} while (0)

	switch (field_id) {
		case SQL_DESC_DATA_PTR:
			base = rec->data_ptr;
			if (desc->bind_type == SQL_BIND_BY_COLUMN) {
				elem_size = (size_t)rec->octet_length;
				offt = 0;
			} else { /* by row */
				ROW_OFFSETS;
			}
			break;
		case SQL_DESC_INDICATOR_PTR:
			base = rec->indicator_ptr;
			if (desc->bind_type == SQL_BIND_BY_COLUMN) {
				elem_size = sizeof(*rec->indicator_ptr);
				offt = 0;
			} else { /* by row */
				ROW_OFFSETS;
			}
			break;
		case SQL_DESC_OCTET_LENGTH_PTR:
			base = rec->octet_length_ptr;
			if (desc->bind_type == SQL_BIND_BY_COLUMN) {
				elem_size = sizeof(*rec->octet_length_ptr);
				offt = 0;
			} else { /* by row */
				ROW_OFFSETS;
			}
			break;
		default:
			BUG("can't calculate the deferred address of field type %d.",
				field_id);
			return NULL;
	}
#undef ROW_OFFSETS

	DBGH(desc->hdr.stmt, "rec@0x%p, field_id:%d : base@0x%p, offset=%d, "
		"elem_size=%zd", rec, field_id, base, offt, elem_size);

	return base ? (char *)base + offt + pos * elem_size : NULL;
}

/*
 * Handles the lengths of the data to copy out to the application:
 * (1) returns the max amount of bytes to copy (in the data_ptr), taking into
 *     account size of data and of buffer, relevant statement attribute and
 *     buffer type;
 * (2) indicates if truncation occured into 'state'.
 * WARN: only to be used with ARD.meta_type == STR || BIN (as it can indicate
 * a size to copy smaller than the original -- truncating).
 */
static size_t buff_octet_size(
	size_t avail, /* how many bytes are there to copy out */
	size_t unit_size, /* the unit size of the buffer (i.e. sizeof(wchar_t)) */
	esodbc_rec_st *arec, esodbc_rec_st *irec,
	esodbc_state_et *state /* out param: only written when truncating */)
{
	esodbc_stmt_st *stmt = arec->desc->hdr.stmt;
	/* how large (bytes) is the buffer to copy into*/
	size_t room = (size_t)arec->octet_length;
	/* statement attribute SQL_ATTR_MAX_LENGTH value */
	size_t attr_max = stmt->max_length;
	/* meta type of IRD */
	esodbc_metatype_et ird_mt = irec->meta_type;
	size_t max_copy, max;

	/* type is signed, driver should not allow a negative to this point:
	 * making sure the cast above is sane. */
	assert(0 <= arec->octet_length);

	/* truncate to statment max bytes, only if "the column contains character
	 * or binary data" */
	max = (ird_mt == METATYPE_STRING || ird_mt == METATYPE_BIN) ? attr_max : 0;

	/* apply "network" truncation first, if need to */
	if (0 < max && max < avail) {
		INFO("applying 'network' truncation %zd -> %zd.", avail, max);
		max_copy = max;
		/* no truncation indicated for this case */
	} else {
		max_copy = avail;
	}

	/* is target buffer to small? adjust size if so and indicate truncation */
	/* Note: this should only be tested/applied if ARD.meta_type == STR||BIN */
	// FIXME: check note above
	if (room < max_copy) {
		INFO("applying buffer truncation %zd -> %zd.", max_copy, room);
		max_copy = room;
		*state = SQL_STATE_01004;
	}

	/* adjust to align to target buffer unit */
	if (max_copy % unit_size) {
		max_copy -= max_copy % unit_size;
	}

	DBG("avail=%zd, room=%zd, attr_max=%zd, metatype:%d => "
		"max_copy=%zd, state=%d.",
		avail, room, attr_max, ird_mt, max_copy, *state);
	return max_copy;
}

/*
 * Indicate the amount of data available to the application, taking into
 * account: the type of data, should truncation - due to max length attr
 * setting - need to be indicated, since original length is indicated, w/o
 * possible buffer truncation, but with possible 'network' truncation.
 */
static inline void write_out_octets(
	SQLLEN *octet_len_ptr, /* buffer to write the avail octets into */
	size_t avail, /* amount of bytes avail */
	esodbc_rec_st *irec)
{
	esodbc_stmt_st *stmt = irec->desc->hdr.stmt;
	/* statement attribute SQL_ATTR_MAX_LENGTH value */
	size_t attr_max = stmt->max_length;
	/* meta type of IRD */
	esodbc_metatype_et ird_mt = irec->meta_type;
	size_t max;

	if (! octet_len_ptr) {
		DBG("NULL octet len pointer, length (%zd) not indicated.", avail);
		return;
	}

	/* truncate to statment max bytes, only if "the column contains character
	 * or binary data" */
	max = (ird_mt == METATYPE_STRING || ird_mt == METATYPE_BIN) ? attr_max : 0;

	if (0 < max) {
		/* put the value of SQL_ATTR_MAX_LENGTH attribute..  even
		 * if this would be larger than what the data actually
		 * occupies after conversion: "the driver has no way of
		 * figuring out what the actual length is" */
		*octet_len_ptr = max;
		DBG("max length (%zd) attribute enforced.", max);
	} else {
		/* if no "network" truncation done, indicate data's length, no
		 * matter if truncated to buffer's size or not */
		*octet_len_ptr = avail;
	}

	DBG("length of data available for transfer: %ld", *octet_len_ptr);
}

/* if an application doesn't specify the conversion, use column's type */
static inline SQLSMALLINT get_c_target_type(esodbc_rec_st *arec,
	esodbc_rec_st *irec)
{
	SQLSMALLINT ctype;
	/* "To use the default mapping, an application specifies the SQL_C_DEFAULT
	 * type identifier." */
	if (arec->type != SQL_C_DEFAULT) {
		ctype = arec->type;
	} else {
		ctype = irec->es_type->c_concise_type;
	}
	DBGH(arec->desc, "target data type: %d.", ctype);
	return ctype;
}

/* transfer to the application a 0-terminated (but unaccounted for) wstr_st */
static SQLRETURN transfer_wstr0(esodbc_rec_st *arec, esodbc_rec_st *irec,
	wstr_st *src, void *data_ptr, SQLLEN *octet_len_ptr)
{
	size_t in_bytes;
	esodbc_state_et state;
	SQLWCHAR *dst;
	esodbc_stmt_st *stmt = arec->desc->hdr.stmt;

	/* the source string must be 0-term'd (since this needs to be transfered
	 * out to the application) */
	assert(src->str[src->cnt] == 0);

	/* always return the app the untruncated number of bytes */
	write_out_octets(octet_len_ptr, src->cnt * sizeof(*src->str), irec);

	if (data_ptr) {
		dst = (SQLWCHAR *)data_ptr;
		state = SQL_STATE_00000;
		in_bytes = buff_octet_size((src->cnt + 1) * sizeof(*src->str),
				sizeof(*src->str), arec, irec, &state);

		if (in_bytes) {
			memcpy(dst, src->str, in_bytes);
			/* TODO: should the left be filled with spaces? :
			 * https://docs.microsoft.com/en-us/sql/odbc/reference/appendixes/rules-for-conversions */

			if (state != SQL_STATE_00000) {
				/* 0-term the buffer */
				((SQLWCHAR *)data_ptr)[(in_bytes/sizeof(SQLWCHAR)) - 1] = 0;
				DBGH(stmt, "aREC@0x%p: `" LWPDL "` transfered truncated as "
					"`%s`.", arec, LWSTR(src), dst);
				RET_HDIAGS(stmt, state);
			} else {
				assert(((SQLWCHAR *)data_ptr)[(in_bytes/sizeof(SQLWCHAR))-1]
					== 0);
				DBGH(stmt, "aREC@0x%p: `" LWPDL "` transfered @ "
					"data_ptr@0x%p.", arec, LWSTR(src), dst);
			}
		}
	} else {
		DBGH(stmt, "aREC@0x%p: NULL transfer buffer.", arec);
	}

	return SQL_SUCCESS;
}

/* transfer to the application a 0-terminated (but unaccounted for) cstr_st */
static SQLRETURN transfer_cstr0(esodbc_rec_st *arec, esodbc_rec_st *irec,
	cstr_st *src, void *data_ptr, SQLLEN *octet_len_ptr)
{
	size_t in_bytes;
	esodbc_state_et state;
	SQLCHAR *dst = (SQLCHAR *)data_ptr;
	esodbc_stmt_st *stmt = arec->desc->hdr.stmt;

	/* the source string must be 0-term'd (since this needs to be transfered
	 * out to the application) */
	assert(src->str[src->cnt] == 0);

	/* always return the app the untruncated number of bytes */
	write_out_octets(octet_len_ptr, src->cnt * sizeof(*src->str), irec);

	if (data_ptr) {
		dst = (SQLCHAR *)data_ptr;
		state = SQL_STATE_00000;
		in_bytes = buff_octet_size((src->cnt + 1) * sizeof(*src->str),
				sizeof(*src->str), arec, irec, &state);

		if (in_bytes) {
			memcpy(dst, src->str, in_bytes);
			/* TODO: should the left be filled with spaces? :
			 * https://docs.microsoft.com/en-us/sql/odbc/reference/appendixes/rules-for-conversions */

			if (state != SQL_STATE_00000) {
				/* 0-term the buffer */
				dst[(in_bytes/sizeof(SQLCHAR)) - 1] = 0;
				DBGH(stmt, "aREC@0x%p: `" LCPDL "` transfered truncated as "
					"`%s`.", arec, LCSTR(src), dst);
				RET_HDIAGS(stmt, state);
			} else {
				assert(dst[(in_bytes/sizeof(SQLCHAR)) - 1] == 0);
				DBGH(stmt, "aREC@0x%p: `" LCPDL "` transfered @ "
					"data_ptr@0x%p.", arec, LCSTR(src), dst);
			}
		}
	} else {
		DBGH(stmt, "aREC@0x%p: NULL transfer buffer.", arec);
	}

	return SQL_SUCCESS;
}

/* 10^n */
static inline unsigned long long pow10(int n)
{
	unsigned long long pow = 1;
	pow <<= n;
	while (n--) {
		pow += pow << 2;
	}
	return pow;
}

static SQLRETURN double_to_numeric(esodbc_rec_st *arec, esodbc_rec_st *irec,
	double src, void *dst)
{
	SQL_NUMERIC_STRUCT *numeric;
	esodbc_stmt_st *stmt;
	SQLSMALLINT prec/*..ision*/;
	unsigned long long ullng;
	long long llng;

	stmt = arec->desc->hdr.stmt;
	numeric = (SQL_NUMERIC_STRUCT *)dst;
	assert(numeric);

	numeric->scale = (SQLCHAR)arec->scale;
	numeric->sign = 0 <= src;

	ullng = numeric->sign ? (unsigned long long)src : (unsigned long long)-src;
	/* =~ log10(abs(src)) */
	for (prec = 0 ; ullng; prec ++) {
		ullng /= 10;
	}
	DBGH(stmt, "arec@0x%p, arec->scale=%d", arec, arec->scale);
	if (arec->scale < 0) {
		llng = (long long)(src / pow10(arec->scale));
		prec -= arec->scale;
	} else if (0 < arec->scale) {
		llng = (long long)(src * pow10(arec->scale));
		prec += arec->scale;
	} else {
		llng = (long long)src;
	}
	ullng = numeric->sign ? (unsigned long long)llng :
		(unsigned long long)-llng;

	if ((0 < arec->precision && arec->precision < prec)
		|| (UCHAR_MAX < prec))  {
		/* precision of source is higher than requested => overflow */
		ERRH(stmt, "conversion overflow. source: %f; requested: "
			"precisions: %d, scale: %d.", src, arec->precision, arec->scale);
		RET_HDIAGS(stmt, SQL_STATE_22003);
	} else if (prec < 0) {
		prec = 0;
		assert(ullng == 0);
	}
	numeric->precision = (SQLCHAR)prec;


#if REG_DWORD != REG_DWORD_LITTLE_ENDIAN
	ullng = _byteswap_ulong(ullng);
#endif /* LE */
	assert(sizeof(ullng) <= sizeof(numeric->val));
	memcpy(numeric->val, (char *)&ullng, sizeof(ullng));
	memset(numeric->val+sizeof(ullng), 0, sizeof(numeric->val)-sizeof(ullng));

	DBGH(stmt, "double %.15f converted to numeric: .sign=%d, precision=%d "
		"(req: %d), .scale=%d (req: %d), .val:`" LCPDL "` (0x%lx).", src,
		numeric->sign, numeric->precision, arec->precision,
		numeric->scale, arec->scale, (int)sizeof(numeric->val), numeric->val,
		ullng);
	return SQL_SUCCESS;
}

/*
 * https://docs.microsoft.com/en-us/sql/odbc/reference/appendixes/transferring-data-in-its-binary-form
 */
static SQLRETURN llong_to_binary(esodbc_rec_st *arec, esodbc_rec_st *irec,
	long long src, void *dst, SQLLEN *src_len)
{
	size_t cnt;
	char *s = (char *)&src;
	esodbc_state_et state = SQL_STATE_00000;
	esodbc_stmt_st *stmt = arec->desc->hdr.stmt;
	unsigned long long ull = src < 0 ? -src : src;

	/* UJ4C uses long long for any integer type -> find out the
	 * smallest type that would accomodate the value (since fixed negatives
	 * would take more space then minimally required). */
	if (ull < CHAR_MAX) {
		cnt = sizeof(char);
	} else if (ull < SHRT_MAX) {
		cnt = sizeof(short);
	} else if (ull < INT_MAX) {
		cnt = sizeof(int);
	} else if (ull < LONG_MAX) {
		cnt = sizeof(long);
	} else { /* definetely ull < LLONG_MAX */
		cnt = sizeof(long long);
	}

	cnt = buff_octet_size(cnt, sizeof(*s), arec, irec, &state);
	if (state) { /* has it been shrunk? */
		REJECT_AS_OOR(stmt, src, /*fixed?*/TRUE, "[BINARY]<[value]");
	}

	if (dst) {
		/* copy bytes as-are: the reverse conversion need to take place on
		 * "same DBMS and hardare platform". */
		memcpy(dst, s, cnt);
		//TODO: should the driver clear all the received buffer?? Cfg option?
		//memset((char *)dst + cnt, 0, arec->octet_length - cnt);
	}
	write_out_octets(src_len, cnt, irec);
	DBGH(stmt, "long long value %lld, converted on %zd octets.", src, cnt);

	return SQL_SUCCESS;
}

static SQLRETURN longlong_to_str(esodbc_rec_st *arec, esodbc_rec_st *irec,
	long long ll, void *data_ptr, SQLLEN *octet_len_ptr, BOOL wide)
{
	/* buffer is overprovisioned for !wide, but avoids double declaration */
	SQLCHAR buff[(ESODBC_PRECISION_INT64 + /*0-term*/1 + /*+/-*/1)
		* sizeof(SQLWCHAR)];
	size_t cnt;
	SQLRETURN ret;
	esodbc_stmt_st *stmt = arec->desc->hdr.stmt;

	cnt = i64tot((int64_t)ll, buff, wide);

	if (wide) {
		wstr_st llwstr = {.str = (SQLWCHAR *)buff, .cnt = cnt};
		ret = transfer_wstr0(arec, irec, &llwstr, data_ptr, octet_len_ptr);
		DBGH(stmt, "long long %lld convertible to w-string `" LWPD "` on "
			"%zd octets.", ll, (SQLWCHAR *)buff, cnt);
	} else {
		cstr_st llcstr = {.str = buff, .cnt = cnt};
		ret = transfer_cstr0(arec, irec, &llcstr, data_ptr, octet_len_ptr);
		DBGH(stmt, "long long %lld convertible to string `" LCPD "` on "
			"%zd octets.", ll, (SQLCHAR *)buff, cnt);
	}

	/* need to change the error code from truncation to "out of
	 * range", since "whole digits" are truncated */
	if (ret == SQL_SUCCESS_WITH_INFO &&
		HDRH(stmt)->diag.state == SQL_STATE_01004) {
		REJECT_AS_OOR(stmt, ll, /*fixed?*/TRUE, "[STRING]<[value]");
	}
	return SQL_SUCCESS;
}

static SQLRETURN copy_longlong(esodbc_rec_st *arec, esodbc_rec_st *irec,
	SQLULEN pos, long long ll)
{
	esodbc_stmt_st *stmt;
	void *data_ptr;
	SQLLEN *octet_len_ptr;
	esodbc_desc_st *ard, *ird;
	SQLRETURN ret;

	stmt = arec->desc->hdr.stmt;
	ird = stmt->ird;
	ard = stmt->ard;

	/* pointer where to write how many characters we will/would use */
	octet_len_ptr = deferred_address(SQL_DESC_OCTET_LENGTH_PTR, pos, arec);
	/* pointer to app's buffer */
	data_ptr = deferred_address(SQL_DESC_DATA_PTR, pos, arec);

	/* Assume a C type behind an SQL C type, but check size representation.
	 * Note: won't work if _min==0 is a legit limit */
#	define REJECT_IF_OOR(_stmt, _ll, _min, _max, _sqlctype, _ctype) \
	do { \
		assert(sizeof(_sqlctype) == sizeof(_ctype)); \
		if ((_min && _ll < _min) || _max < _ll) { \
			REJECT_AS_OOR(_stmt, _ll, /*fixed int*/TRUE, _ctype); \
		} \
	} while (0)
	/* Transfer a long long to an SQL integer type.
	 * Uses local vars: stmt, data_ptr, irec, octet_len_ptr. */
#	define TRANSFER_LL(_ll, _min, _max, _sqlctype, _ctype) \
	do { \
		REJECT_IF_NULL_DEST_BUFF(stmt, data_ptr); \
		REJECT_IF_OOR(stmt, _ll, _min, _max, _sqlctype, _ctype); \
		*(_sqlctype *)data_ptr = (_sqlctype)_ll; \
		write_out_octets(octet_len_ptr, sizeof(_sqlctype), irec); \
		DBGH(stmt, "converted long long %lld to " STR(_sqlctype) " 0x%llx.", \
			_ll, (intptr_t)*(_sqlctype *)data_ptr); \
	} while (0)

	switch (get_c_target_type(arec, irec)) {
		case SQL_C_CHAR:
			return longlong_to_str(arec, irec, ll, data_ptr, octet_len_ptr,
					FALSE);
		case SQL_C_WCHAR:
			return longlong_to_str(arec, irec, ll, data_ptr, octet_len_ptr,
					TRUE);

		case SQL_C_TINYINT:
		case SQL_C_STINYINT:
			TRANSFER_LL(ll, CHAR_MIN, CHAR_MAX, SQLSCHAR, char);
			break;
		case SQL_C_UTINYINT:
			TRANSFER_LL(ll, 0, UCHAR_MAX, SQLCHAR, unsigned char);
			break;
		case SQL_C_SHORT:
		case SQL_C_SSHORT:
			TRANSFER_LL(ll, SHRT_MIN, SHRT_MAX, SQLSMALLINT, short);
			break;
		case SQL_C_USHORT:
			TRANSFER_LL(ll, 0, USHRT_MAX, SQLUSMALLINT, unsigned short);
			break;
		case SQL_C_LONG:
		case SQL_C_SLONG:
			TRANSFER_LL(ll, LONG_MIN, LONG_MAX, SQLINTEGER, long);
			break;
		case SQL_C_ULONG:
			TRANSFER_LL(ll, 0, ULONG_MAX, SQLUINTEGER, unsigned long);
			break;
		case SQL_C_SBIGINT:
			TRANSFER_LL(ll, LLONG_MIN, LLONG_MAX, SQLBIGINT, long long);
			break;
		case SQL_C_UBIGINT:
			TRANSFER_LL(ll, 0, ULLONG_MAX, SQLUBIGINT, unsigned long long);
			break;

		case SQL_C_BIT:
			REJECT_IF_NULL_DEST_BUFF(stmt, data_ptr);
			if (ll < 0 || 2 <= ll) {
				REJECT_AS_OOR(stmt, ll, /*fixed int*/TRUE, SQL_C_BIT);
			} else { /* 0 or 1 */
				*(SQLCHAR *)data_ptr = (SQLCHAR)ll;
			}
			write_out_octets(octet_len_ptr, sizeof(SQLSCHAR), irec);
			break;

		case SQL_C_NUMERIC:
			REJECT_IF_NULL_DEST_BUFF(stmt, data_ptr);
			ret = double_to_numeric(arec, irec, (double)ll, data_ptr);
			if (! SQL_SUCCEEDED(ret)) {
				return ret;
			}
			write_out_octets(octet_len_ptr, sizeof(SQL_NUMERIC_STRUCT), irec);
			break;

		case SQL_C_FLOAT:
			REJECT_IF_NULL_DEST_BUFF(stmt, data_ptr);
			REJECT_IF_OOR(stmt, ll, -FLT_MAX, FLT_MAX, SQLREAL, float);
			*(SQLREAL *)data_ptr = (SQLREAL)ll;
			write_out_octets(octet_len_ptr, sizeof(SQLREAL), irec);
			break;

		case SQL_C_DOUBLE:
			REJECT_IF_NULL_DEST_BUFF(stmt, data_ptr);
			REJECT_IF_OOR(stmt, ll, -DBL_MAX, DBL_MAX, SQLDOUBLE, double);
			*(SQLDOUBLE *)data_ptr = (SQLDOUBLE)ll;
			write_out_octets(octet_len_ptr, sizeof(SQLDOUBLE), irec);
			break;

		case SQL_C_BINARY:
			return llong_to_binary(arec, irec, ll, data_ptr, octet_len_ptr);

		default:
			BUGH(stmt, "unexpected unhanlded data type: %d.",
				get_c_target_type(arec, irec));
			return SQL_ERROR;
	}
	DBGH(stmt, "REC@0x%p, data_ptr@0x%p, copied long long: %lld.", arec,
		data_ptr, ll);

	return SQL_SUCCESS;

#	undef REJECT_IF_OOR
#	undef TRANSFER_LL
}

static SQLRETURN double_to_bit(esodbc_rec_st *arec, esodbc_rec_st *irec,
	double src, void *data_ptr, SQLLEN *octet_len_ptr)
{
	esodbc_state_et state = SQL_STATE_00000;
	esodbc_stmt_st *stmt = arec->desc->hdr.stmt;

	REJECT_IF_NULL_DEST_BUFF(stmt, data_ptr);

	write_out_octets(octet_len_ptr, sizeof(SQLCHAR), irec);

	if (src < 0 || 2 <= src) {
		REJECT_AS_OOR(stmt, src, /*fixed?*/FALSE, SQL_C_BIT);
	} else if (0 < src && src < 1) {
		*(SQLCHAR *)data_ptr = 0;
		state = SQL_STATE_01S07;
	} else if (1 < src && src < 2) {
		*(SQLCHAR *)data_ptr = 1;
		state = SQL_STATE_01S07;
	} else { /* 0 or 1 */
		*(SQLCHAR *)data_ptr = (SQLCHAR)src;
	}
	if (state != SQL_STATE_00000) {
		INFOH(stmt, "truncating when converting %f as %d.", src,
			*(SQLCHAR *)data_ptr);
		RET_HDIAGS(stmt, state);
	}

	DBGH(stmt, "double %f converted to bit %d.", src, *(SQLCHAR *)data_ptr);

	return SQL_SUCCESS;
}

static SQLRETURN double_to_binary(esodbc_rec_st *arec, esodbc_rec_st *irec,
	double dbl, void *data_ptr, SQLLEN *octet_len_ptr)
{
	size_t cnt;
	double udbl = dbl < 0 ? -dbl : dbl;
	float flt;
	char *ptr;
	esodbc_state_et state = SQL_STATE_00000;
	esodbc_stmt_st *stmt = arec->desc->hdr.stmt;

	if (udbl < FLT_MIN || FLT_MAX < udbl) {
		/* value's precision/scale requires a double */
		cnt = sizeof(dbl);
		ptr = (char *)&dbl;
	} else {
		flt = (float)dbl;
		cnt = sizeof(flt);
		ptr = (char *)&flt;
	}

	cnt = buff_octet_size(cnt, sizeof(*ptr), arec, irec, &state);
	if (state) {
		REJECT_AS_OOR(stmt, dbl, /*fixed?*/FALSE, "[BINARY]<[floating]");
	}
	write_out_octets(octet_len_ptr, cnt, irec);
	if (data_ptr) {
		memcpy(data_ptr, ptr, cnt);
		//TODO: should the driver clear all the received buffer?? Cfg option?
		//memset((char *)data_ptr + cnt, 0, arec->octet_length - cnt);
	}

	DBGH(stmt, "converted double %f to binary on %zd octets.", dbl, cnt);

	return SQL_SUCCESS;
}

static SQLRETURN double_to_str(esodbc_rec_st *arec, esodbc_rec_st *irec,
	double dbl, void *data_ptr, SQLLEN *octet_len_ptr, BOOL wide)
{
	long long whole;
	unsigned long long fraction;
	double rest;
	SQLSMALLINT scale;
	size_t pos, octets;
	/* buffer is overprovisioned for !wide, but avoids double declaration */
	SQLCHAR buff[(2 * ESODBC_PRECISION_INT64 + /*.*/1 + /*\0*/1)
		* sizeof(SQLWCHAR)];
	/* buffer unit size */
	size_t usize = wide ? sizeof(SQLWCHAR) : sizeof(SQLCHAR);
	esodbc_state_et state = SQL_STATE_00000;
	esodbc_stmt_st *stmt = arec->desc->hdr.stmt;

	/*
	 * split the whole and fractional parts
	 */
	assert(sizeof(dbl) == sizeof(whole)); /* [double]==[long long] */
	whole = (long long)dbl;
	rest = dbl - whole;

	/* retain user defined or data source default number of fraction digits */
	scale = 0 < arec->scale ? arec->scale : irec->es_type->maximum_scale;
	rest *= pow10(scale);
	rest = round(rest);
	fraction = rest < 0 ? (unsigned long long) -rest
		: (unsigned long long)rest;

	/* copy integer part into work buffer */
	pos = i64tot((int64_t)whole, buff, wide);
	/* would writing just the whole part + \0 fit into the buffer? */
	octets = buff_octet_size((pos + 1) * usize, usize, arec, irec, &state);
	if (state) {
		REJECT_AS_OOR(stmt, dbl, /*fixed?*/FALSE, "[STRING]<[floating.whole]");
	} else {
		assert(octets == (pos + 1) * usize);
	}

	if (wide) {
		((SQLWCHAR *)buff)[pos ++] = L'.';
	} else {
		((SQLCHAR *)buff)[pos ++] = '.';
	}

	/* copy fractional part into work buffer */
	pos += ui64tot((uint64_t)fraction, (char *)buff + pos * usize, wide);

	/* write how many bytes (w/o \0) we'd write if buffer is large enough */
	write_out_octets(octet_len_ptr, pos * usize, irec);
	/* compute how many bytes we can actually transfer, including \0 */
	octets = buff_octet_size((pos + 1) * usize, usize, arec, irec, &state);

	if (data_ptr) {
		/* transfer the bytes out */
		memcpy(data_ptr, buff, octets);
		if (state) {
			/* usize < octets, since user input is checked above for OOR  */
			if (wide) {
				((SQLWCHAR *)data_ptr)[octets/usize - 1] = L'\0';
			} else {
				((SQLCHAR *)data_ptr)[octets/usize - 1] = '\0';
			}
		}
	}

	if (wide) { /* 15-16 decimals precision for x64 double (TODO: 32b)  */
		DBGH(stmt, "double %.16f converted to w-string `" LWPD "` on %zd "
			"octets (state: %d; scale: %d).", dbl, buff, octets, state, scale);
	} else {
		DBGH(stmt, "double %.16f converted to string `" LCPD "` on %zd "
			"octets (state: %d; scale: %d).", dbl, buff, octets, state, scale);
	}

	if (state) {
		RET_HDIAGS(stmt, state);
	} else {
		return SQL_SUCCESS;
	}
}

static SQLRETURN copy_double(esodbc_rec_st *arec, esodbc_rec_st *irec,
	SQLULEN pos, double dbl)
{
	esodbc_stmt_st *stmt;
	void *data_ptr;
	SQLLEN *octet_len_ptr;
	esodbc_desc_st *ard, *ird;
	SQLRETURN ret;
	double udbl;

	stmt = arec->desc->hdr.stmt;
	ird = stmt->ird;
	ard = stmt->ard;

	/* pointer where to write how many characters we will/would use */
	octet_len_ptr = deferred_address(SQL_DESC_OCTET_LENGTH_PTR, pos, arec);
	/* pointer to app's buffer */
	data_ptr = deferred_address(SQL_DESC_DATA_PTR, pos, arec);

	/* Transfer a double to an SQL integer type.
	 * Uses local vars: stmt, data_ptr, irec, octet_len_ptr.
	 * Returns - RET_ - 01S07 on success (due to truncation of fractionals). */
#	define RET_TRANSFER_DBL(_dbl, _min, _max, _sqlctype, _ctype) \
	do { \
		/* using C type limits, so check C and SQL C type precision */ \
		assert(sizeof(_sqlctype) == sizeof(_ctype)); \
		if (_dbl) { \
			if ((_sqlctype)_dbl < _min || _max < (_sqlctype)_dbl) { \
				REJECT_AS_OOR(stmt, _dbl, /*fixed?*/FALSE, _sqlctype); \
			} \
		} else { \
			double __udbl = dbl < 0 ? -dbl : dbl; \
			if (_max < (_sqlctype)__udbl) { \
				REJECT_AS_OOR(stmt, _dbl, /*fixed?*/FALSE, _sqlctype); \
			} \
		} \
		*(_sqlctype *)data_ptr = (_sqlctype)_dbl; \
		write_out_octets(octet_len_ptr, sizeof(_sqlctype), irec); \
		DBGH(stmt, "converted double %f to " STR(_sqlctype) " 0x%llx.", _dbl, \
			(intptr_t)*(_sqlctype *)data_ptr); \
		RET_HDIAGS(stmt, SQL_STATE_01S07); \
	} while (0)

	switch (get_c_target_type(arec, irec)) {
		case SQL_C_CHAR:
			return double_to_str(arec, irec, dbl, data_ptr, octet_len_ptr,
					FALSE);
		case SQL_C_WCHAR:
			return double_to_str(arec, irec, dbl, data_ptr, octet_len_ptr,
					TRUE);

		case SQL_C_TINYINT:
		case SQL_C_STINYINT:
			RET_TRANSFER_DBL(dbl, CHAR_MIN, CHAR_MAX, SQLSCHAR, char);
		case SQL_C_UTINYINT:
			RET_TRANSFER_DBL(dbl, 0, UCHAR_MAX, SQLCHAR, unsigned char);
		case SQL_C_SBIGINT:
			RET_TRANSFER_DBL(dbl, LLONG_MIN, LLONG_MAX, SQLBIGINT, long long);
		case SQL_C_UBIGINT:
			RET_TRANSFER_DBL(dbl, 0, LLONG_MAX, SQLUBIGINT, long long);
		case SQL_C_SHORT:
		case SQL_C_SSHORT:
			RET_TRANSFER_DBL(dbl, SHRT_MIN, SHRT_MAX, SQLSMALLINT, short);
		case SQL_C_USHORT:
			RET_TRANSFER_DBL(dbl, 0, USHRT_MAX, SQLUSMALLINT, unsigned short);
		case SQL_C_LONG:
		case SQL_C_SLONG:
			RET_TRANSFER_DBL(dbl, LONG_MIN, LONG_MAX, SQLINTEGER, long);
		case SQL_C_ULONG:
			RET_TRANSFER_DBL(dbl, 0, ULONG_MAX, SQLINTEGER, unsigned long);

		case SQL_C_NUMERIC:
			REJECT_IF_NULL_DEST_BUFF(stmt, data_ptr);
			ret = double_to_numeric(arec, irec, dbl, data_ptr);
			if (! SQL_SUCCEEDED(ret)) {
				return ret;
			}
			write_out_octets(octet_len_ptr, sizeof(SQL_NUMERIC_STRUCT), irec);
			break;

		case SQL_C_FLOAT:
			REJECT_IF_NULL_DEST_BUFF(stmt, data_ptr);
			udbl = dbl < 0 ? -dbl : dbl;
			if (udbl < FLT_MIN || FLT_MAX < udbl) {
				REJECT_AS_OOR(stmt, dbl, /* is fixed */FALSE, SQLREAL);
			}
			*(SQLREAL *)data_ptr = (SQLREAL)dbl;
			write_out_octets(octet_len_ptr, sizeof(SQLREAL), irec);
			break;
		case SQL_C_DOUBLE:
			REJECT_IF_NULL_DEST_BUFF(stmt, data_ptr);
			*(SQLDOUBLE *)data_ptr = dbl;
			write_out_octets(octet_len_ptr, sizeof(SQLDOUBLE), irec);
			break;

		case SQL_C_BIT:
			return double_to_bit(arec, irec, dbl, data_ptr, octet_len_ptr);

		case SQL_C_BINARY:
			return double_to_binary(arec, irec, dbl, data_ptr, octet_len_ptr);

		default:
			BUGH(stmt, "unexpected unhanlded data type: %d.",
				get_c_target_type(arec, irec));
			return SQL_ERROR;
	}

	DBGH(stmt, "REC@0x%p, data_ptr@0x%p, copied double: %f.", arec,
		data_ptr, dbl);

	return SQL_SUCCESS;

#	undef RET_TRANSFER_DBL
}

static SQLRETURN copy_boolean(esodbc_rec_st *arec, esodbc_rec_st *irec,
	SQLULEN pos, BOOL boolval)
{
	esodbc_stmt_st *stmt;
	void *data_ptr;
	SQLLEN *octet_len_ptr;
	wstr_st wbool;
	cstr_st cbool;

	stmt = arec->desc->hdr.stmt;

	/* pointer where to write how many bytes we will/would use */
	octet_len_ptr = deferred_address(SQL_DESC_OCTET_LENGTH_PTR, pos, arec);
	/* pointer to app's buffer */
	data_ptr = deferred_address(SQL_DESC_DATA_PTR, pos, arec);

	switch (get_c_target_type(arec, irec)) {
		case SQL_C_WCHAR:
			if (arec->octet_length < 1) { /* can't inquiry needed buffer len */
				REJECT_AS_OOR(stmt, boolval, /*fixed int*/TRUE, NULL WCHAR);
			}
			wbool = boolval ? MK_WSTR("true") : MK_WSTR("false");
			return transfer_wstr0(arec, irec, &wbool, data_ptr, octet_len_ptr);
		case SQL_C_CHAR:
			if (arec->octet_length < 1) { /* can't inquiry needed buffer len */
				REJECT_AS_OOR(stmt, boolval, /*fixed int*/TRUE, NULL CHAR);
			}
			cbool = boolval ? MK_CSTR("true") : MK_CSTR("false");
			return transfer_cstr0(arec, irec, &cbool, data_ptr, octet_len_ptr);
		default:
			return copy_longlong(arec, irec, pos, boolval ? 1LL : 0LL);
	}

	DBGH(stmt, "REC@0x%p, data_ptr@0x%p, copied boolean: `%d`.", arec,
		data_ptr, boolval);
	return SQL_SUCCESS;
}

/*
 * -> SQL_C_CHAR
 * Note: chars_0 param accounts for 0-term, but length indicated back to the
 * application must not.
 */
static SQLRETURN wstr_to_cstr(esodbc_rec_st *arec, esodbc_rec_st *irec,
	void *data_ptr, SQLLEN *octet_len_ptr,
	const wchar_t *wstr, size_t chars_0)
{
	esodbc_state_et state = SQL_STATE_00000;
	esodbc_stmt_st *stmt = arec->desc->hdr.stmt;
	char *charp;
	int in_bytes, out_bytes, c;

	if (data_ptr) {
		charp = (char *)data_ptr;

		in_bytes = (int)buff_octet_size(chars_0 * sizeof(*wstr),
				sizeof(*charp), arec, irec, &state);
		/* trim the original string until it fits in output buffer, with given
		 * length limitation */
		for (c = (int)chars_0; 0 < c; c --) {
			out_bytes = WCS2U8(wstr, c, charp, in_bytes);
			if (out_bytes <= 0) {
				if (WCS2U8_BUFF_INSUFFICIENT) {
					continue;
				}
				ERRNH(stmt, "failed to convert wchar* to char* for string `"
					LWPDL "`.", chars_0, wstr);
				RET_HDIAGS(stmt, SQL_STATE_22018);
			} else {
				/* conversion succeeded */
				break;
			}
		}

		/* if 0's present => 0 < out_bytes */
		assert(wstr[chars_0 - 1] == L'\0');
		assert(0 < out_bytes);
		/* is user gives 0 as buffer size, out_bytes will also be 0 */
		if (charp[out_bytes - 1]) {
			/* ran out of buffer => not 0-terminated and truncated already */
			charp[out_bytes - 1] = 0;
			state = SQL_STATE_01004; /* indicate truncation */
		}

		DBGH(stmt, "REC@0x%p, data_ptr@0x%p, copied %zd bytes: `" LWPD "`.",
			arec, data_ptr, out_bytes, charp);
	} else {
		DBGH(stmt, "REC@0x%p, NULL data_ptr.", arec);
	}

	/* if length needs to be given, calculate it (not truncated) & converted */
	if (octet_len_ptr) {
		out_bytes = (size_t)WCS2U8(wstr, (int)chars_0, NULL, 0);
		if (out_bytes <= 0) {
			ERRNH(stmt, "failed to convert wchar* to char* for string `"
				LWPDL "`.", chars_0, wstr);
			RET_HDIAGS(stmt, SQL_STATE_22018);
		} else {
			/* chars_0 accounts for 0-terminator, so WCS2U8 will count that in
			 * the output as well => trim it, since we must not count it when
			 * indicating the length to the application */
			out_bytes --;
		}
		write_out_octets(octet_len_ptr, out_bytes, irec);
	} else {
		DBGH(stmt, "REC@0x%p, NULL octet_len_ptr.", arec);
	}

	if (state != SQL_STATE_00000) {
		RET_HDIAGS(stmt, state);
	}
	return SQL_SUCCESS;
}

/*
 * -> SQL_C_WCHAR
 * Note: chars_0 accounts for 0-term, but length indicated back to the
 * application must not.
 */
static SQLRETURN wstr_to_wstr(esodbc_rec_st *arec, esodbc_rec_st *irec,
	void *data_ptr, SQLLEN *octet_len_ptr,
	const wchar_t *wstr, size_t chars_0)
{
	wstr_st wsrc = {(SQLWCHAR *)wstr, chars_0 - 1};
	return transfer_wstr0(arec, irec, &wsrc, data_ptr, octet_len_ptr);
}

/* function expects chars not to count the 0-term */
static BOOL wstr_to_timestamp_struct(const wchar_t *wstr, size_t chars,
	TIMESTAMP_STRUCT *tss)
{
	char buff[sizeof(ESODBC_ISO8601_TEMPLATE)/*+\0*/];
	int len;
	timestamp_t tsp;
	struct tm tmp;

	DBG("converting ISO 8601 `" LWPDL "` to timestamp.", chars, wstr);

	if (sizeof(buff) - 1 < chars) {
		ERR("`" LWPDL "` not a TIMESTAMP.", chars, wstr);
		return FALSE;
	}
	len = ansi_w2c(wstr, buff, chars);
	/* len counts the 0-term */
	if (len <= 0 || timestamp_parse(buff, len - 1, &tsp) ||
		(! timestamp_to_tm_local(&tsp, &tmp))) {
		ERR("data `" LWPDL "` not an ANSI ISO 8601 format.", chars, wstr);
		return FALSE;
	}
	TM_TO_TIMESTAMP_STRUCT(&tmp, tss);
	tss->fraction = tsp.nsec / 1000000;

	DBG("parsed ISO 8601: `%d-%d-%dT%d:%d:%d.%u+%dm`.",
		tss->year, tss->month, tss->day,
		tss->hour, tss->minute, tss->second, tss->fraction,
		tsp.offset);

	return TRUE;
}


/*
 * -> SQL_C_TYPE_TIMESTAMP
 *
 * Conversts an ES/SQL 'date' or a text representation of a
 * timestamp/date/time value into a TIMESTAMP_STRUCT (indicates the detected
 * input format into the "format" parameter).
 */
static SQLRETURN wstr_to_timestamp(esodbc_rec_st *arec, esodbc_rec_st *irec,
	void *data_ptr, SQLLEN *octet_len_ptr,
	const wchar_t *wstr, size_t chars_0, SQLSMALLINT *format)
{
	size_t cnt = chars_0 - 1;
	esodbc_stmt_st *stmt = arec->desc->hdr.stmt;
	TIMESTAMP_STRUCT *tss = (TIMESTAMP_STRUCT *)data_ptr;
	SQLWCHAR buff[] = MK_WPTR("1000-10-10T10:10:10.1001001Z");

	if (octet_len_ptr) {
		*octet_len_ptr = sizeof(*tss);
	}

	if (data_ptr) {
		/* right & left trim the data before attempting conversion */
		wstr = trim_ws(wstr, &cnt);

		switch (irec->concise_type) {
			case SQL_TYPE_TIMESTAMP:
				if (! wstr_to_timestamp_struct(wstr, cnt, tss)) {
					RET_HDIAGS(stmt, SQL_STATE_22018);
				}
				if (format) {
					*format = SQL_TYPE_TIMESTAMP;
				}
				break;
			case SQL_VARCHAR:
				if (sizeof(ESODBC_TIME_TEMPLATE) - 1 < cnt) {
					/* longer than a date-value -> try a timestamp */
					if (! wstr_to_timestamp_struct(wstr, cnt, tss)) {
						RET_HDIAGS(stmt, SQL_STATE_22018);
					}
					if (format) {
						*format = SQL_TYPE_TIMESTAMP;
					}
				} else if (/*hh:mm:ss*/8 <= cnt && wstr[2] == L':' &&
					wstr[5] == L':') { /* could this be a time-val? */
					/* copy active value in template and parse it as TS */
					/* copy is safe: cnt <= [time template] < [buff] */
					wcsncpy(buff + sizeof(ESODBC_DATE_TEMPLATE) - 1, wstr,
						cnt);
					/* there could be a varying number of fractional digits */
					buff[sizeof(ESODBC_DATE_TEMPLATE) - 1 + cnt] = L'Z';
					if (! wstr_to_timestamp_struct(buff,
							sizeof(ESODBC_DATE_TEMPLATE) + cnt, tss)) {
						ERRH(stmt, "`" LWPDL "` not a TIME.", cnt, wstr);
						RET_HDIAGS(stmt, SQL_STATE_22018);
					} else {
						tss->year = tss->month = tss->day = 0;
						if (format) {
							*format = SQL_TYPE_TIME;
						}
					}
				} else if (/*yyyy-mm-dd*/10 <= cnt && wstr[4] == L'-' &&
					wstr[7] == L'-') { /* could this be a date-val? */
					/* copy active value in template and parse it as TS */
					/* copy is safe: cnt <= [time template] < [buff] */
					wcsncpy(buff, wstr, cnt);
					if (! wstr_to_timestamp_struct(buff,
							sizeof(buff)/sizeof(buff[0]) - 1, tss)) {
						ERRH(stmt, "`" LWPDL "` not a DATE.", cnt, wstr);
						RET_HDIAGS(stmt, SQL_STATE_22018);
					} else {
						tss->hour = tss->minute = tss->second = 0;
						tss->fraction = 0;
						if (format) {
							*format = SQL_TYPE_DATE;
						}
					}
				} else {
					ERRH(stmt, "`" LWPDL "` not a DATE/TIME/Timestamp.", cnt,
						wstr);
					RET_HDIAGS(stmt, SQL_STATE_22018);
				}
				break;

			case SQL_CHAR:
			case SQL_LONGVARCHAR:
			case SQL_WCHAR:
			case SQL_WLONGVARCHAR:
			case SQL_TYPE_DATE:
			case SQL_TYPE_TIME:
				BUGH(stmt, "unexpected (but permitted) SQL type.");
				RET_HDIAGS(stmt, SQL_STATE_HY004);
			default:
				BUGH(stmt, "uncought invalid conversion.");
				RET_HDIAGS(stmt, SQL_STATE_07006);
		}
	} else {
		DBGH(stmt, "REC@0x%p, NULL data_ptr", arec);
	}

	return SQL_SUCCESS;
}

/*
 * -> SQL_C_TYPE_DATE
 */
static SQLRETURN wstr_to_date(esodbc_rec_st *arec, esodbc_rec_st *irec,
	void *data_ptr, SQLLEN *octet_len_ptr,
	const wchar_t *wstr, size_t chars_0)
{
	esodbc_stmt_st *stmt = arec->desc->hdr.stmt;
	DATE_STRUCT *ds = (DATE_STRUCT *)data_ptr;
	TIMESTAMP_STRUCT tss;
	SQLRETURN ret;
	SQLSMALLINT fmt;

	if (octet_len_ptr) {
		*octet_len_ptr = sizeof(*ds);
	}

	if (data_ptr) {
		ret = wstr_to_timestamp(arec, irec, &tss, NULL, wstr, chars_0, &fmt);
		if (! SQL_SUCCEEDED(ret)) {
			return ret;
		}
		if (fmt == SQL_TYPE_TIME) {
			/* it's a time-value */
			RET_HDIAGS(stmt, SQL_STATE_22018);
		}
		ds->year = tss.year;
		ds->month = tss.month;
		ds->day = tss.day;
		if (tss.hour || tss.minute || tss.second || tss.fraction) {
			/* value's truncated */
			RET_HDIAGS(stmt, SQL_STATE_01S07);
		}
	} else {
		DBGH(stmt, "REC@0x%p, NULL data_ptr", arec);
	}

	return SQL_SUCCESS;
}

/*
 * -> SQL_C_TYPE_TIME
 */
static SQLRETURN wstr_to_time(esodbc_rec_st *arec, esodbc_rec_st *irec,
	void *data_ptr, SQLLEN *octet_len_ptr,
	const wchar_t *wstr, size_t chars_0)
{
	esodbc_stmt_st *stmt = arec->desc->hdr.stmt;
	TIME_STRUCT *ts = (TIME_STRUCT *)data_ptr;
	TIMESTAMP_STRUCT tss;
	SQLRETURN ret;
	SQLSMALLINT fmt;

	if (octet_len_ptr) {
		*octet_len_ptr = sizeof(*ts);
	}

	if (data_ptr) {
		ret = wstr_to_timestamp(arec, irec, &tss, NULL, wstr, chars_0, &fmt);
		if (! SQL_SUCCEEDED(ret)) {
			return ret;
		}
		/* need to differentiate between:
		 * - 1234-12-34T00:00:00Z : valid and
		 * - 1234-12-34 : invalid */
		if (fmt == SQL_TYPE_DATE) {
			RET_HDIAGS(stmt, SQL_STATE_22018);
		}
		ts->hour = tss.hour;
		ts->minute = tss.minute;
		ts->second = tss.second;
		if (tss.fraction) {
			/* value's truncated */
			RET_HDIAGS(stmt, SQL_STATE_01S07);
		}
	} else {
		DBGH(stmt, "REC@0x%p, NULL data_ptr", arec);
	}

	return SQL_SUCCESS;
}

/*
 * wstr: is 0-terminated and terminator is counted in 'chars_0'.
 * However: "[w]hen C strings are used to hold character data, the
 * null-termination character is not considered to be part of the data and is
 * not counted as part of its byte length."
 * "If the data was converted to a variable-length data type, such as
 * character or binary [...][i]t then null-terminates the data."
 */
static SQLRETURN copy_string(esodbc_rec_st *arec, esodbc_rec_st *irec,
	SQLULEN pos, const wchar_t *wstr, size_t chars_0)
{
	esodbc_stmt_st *stmt;
	void *data_ptr;
	SQLLEN *octet_len_ptr;
	esodbc_desc_st *ard, *ird;

	stmt = arec->desc->hdr.stmt;
	ird = stmt->ird;
	ard = stmt->ard;

	/* pointer where to write how many characters we will/would use */
	octet_len_ptr = deferred_address(SQL_DESC_OCTET_LENGTH_PTR, pos, arec);
	/* pointer to app's buffer */
	data_ptr = deferred_address(SQL_DESC_DATA_PTR, pos, arec);

	switch (get_c_target_type(arec, irec)) {
		case SQL_C_CHAR:
			return wstr_to_cstr(arec, irec, data_ptr, octet_len_ptr,
					wstr, chars_0);
		case SQL_C_WCHAR:
			return wstr_to_wstr(arec, irec, data_ptr, octet_len_ptr,
					wstr, chars_0);

		case SQL_C_TYPE_TIMESTAMP:
			return wstr_to_timestamp(arec, irec, data_ptr, octet_len_ptr,
					wstr, chars_0, NULL);
		case SQL_C_TYPE_DATE:
			return wstr_to_date(arec, irec, data_ptr, octet_len_ptr,
					wstr, chars_0);
		case SQL_C_TYPE_TIME:
			return wstr_to_time(arec, irec, data_ptr, octet_len_ptr,
					wstr, chars_0);

		default:
			// FIXME: convert data
			FIXME;
	}

	return SQL_SUCCESS;
}

/*
 * Copy one row from IRD to ARD.
 * pos: row number in the rowset
 * Returns: ...
 */
static SQLRETURN copy_one_row(esodbc_stmt_st *stmt, SQLULEN pos, UJObject row)
{
	SQLSMALLINT i;
	SQLLEN rowno;
	SQLRETURN ret;
	UJObject obj;
	void *iter_row;
	SQLLEN *ind_len;
	long long ll;
	double dbl;
	const wchar_t *wstr;
	BOOL boolval;
	size_t len;
	BOOL with_info;
	esodbc_desc_st *ard, *ird;
	esodbc_rec_st *arec, *irec;

	rowno = stmt->rset.frows + pos + /*1-based*/1;
	ard = stmt->ard;
	ird = stmt->ird;

#define RET_ROW_DIAG(_state, _message, _colno) \
	do { \
		if (ard->array_status_ptr) \
			ard->array_status_ptr[pos] = SQL_ROW_ERROR; \
		return post_row_diagnostic(&stmt->hdr.diag, _state, MK_WPTR(_message),\
				0, rowno, _colno); \
	} while (0)
#define SET_ROW_DIAG(_rowno, _colno) \
	do { \
		stmt->hdr.diag.row_number = _rowno; \
		stmt->hdr.diag.column_number = _colno; \
	} while (0)

	if (! UJIsArray(row)) {
		ERRH(stmt, "one '%s' (#%zd) element in result set not array; type:"
			" %d.", JSON_ANSWER_ROWS, stmt->rset.vrows, UJGetType(row));
		RET_ROW_DIAG(SQL_STATE_01S01, MSG_INV_SRV_ANS,
			SQL_NO_COLUMN_NUMBER);
	}
	iter_row = UJBeginArray(row);
	if (! iter_row) {
		ERRH(stmt, "Failed to obtain iterator on row (#%zd): %s.", rowno,
			UJGetError(stmt->rset.state));
		RET_ROW_DIAG(SQL_STATE_01S01, MSG_INV_SRV_ANS,
			SQL_NO_COLUMN_NUMBER);
	}

	with_info = FALSE;
	/* iterate over the contents of one table row */
	for (i = 0; i < ard->count && UJIterArray(&iter_row, &obj); i ++) {
		arec = &ard->recs[i]; /* access safe if 'i < ard->count' */
		/* if record not bound skip it */
		if (! REC_IS_BOUND(arec)) {
			DBGH(stmt, "column #%d not bound, skipping it.", i + 1);
			continue;
		}

		irec = &ird->recs[i]; /* access checked by UJIterArray() condition */

		switch (UJGetType(obj)) {
			default:
				ERRH(stmt, "unexpected object of type %d in row L#%zd/T#%zd.",
					UJGetType(obj), stmt->rset.vrows, stmt->rset.frows);
				RET_ROW_DIAG(SQL_STATE_01S01, MSG_INV_SRV_ANS, i + 1);
			/* RET_.. returns */

			case UJT_Null:
				DBGH(stmt, "value [%zd, %d] is NULL.", rowno, i + 1);
				/* Note: if ever causing an issue, check
				 * arec->es_type->nullable before returning NULL to app */
				ind_len = deferred_address(SQL_DESC_INDICATOR_PTR, pos, arec);
				if (! ind_len) {
					ERRH(stmt, "no buffer to signal NULL value.");
					RET_ROW_DIAG(SQL_STATE_22002, "Indicator variable required"
						" but not supplied", i + 1);
				}
				*ind_len = SQL_NULL_DATA;
				continue; /* instead of break! no 'ret' processing to do. */

			case UJT_String:
				wstr = UJReadString(obj, &len);
				DBGH(stmt, "value [%zd, %d] is string [%d]:`" LWPDL "`.",
					rowno, i + 1, len, len, wstr);
				/* UJSON4C returns chars count, but 0-terminates w/o counting
				 * the terminator */
				assert(wstr[len] == 0);
				/* "When character data is returned from the driver to the
				 * application, the driver must always null-terminate it." */
				ret = copy_string(arec, irec, pos, wstr, len + /*\0*/1);
				break;

			case UJT_Long:
			case UJT_LongLong:
				ll = UJNumericLongLong(obj);
				DBGH(stmt, "value [%zd, %d] is numeric: %lld.", rowno, i + 1,
					ll);
				ret = copy_longlong(arec, irec, pos, ll);
				break;

			case UJT_Double:
				dbl = UJNumericFloat(obj);
				DBGH(stmt, "value [%zd, %d] is double: %f.", rowno, i + 1,
					dbl);
				ret = copy_double(arec, irec, pos, dbl);
				break;

			case UJT_True:
			case UJT_False:
				boolval = UJGetType(obj) == UJT_True ? TRUE : FALSE;
				DBGH(stmt, "value [%zd, %d] is boolean: %d.", rowno, i + 1,
					boolval);
				ret = copy_boolean(arec, irec, pos, boolval);
				break;
		}

		switch (ret) {
			case SQL_SUCCESS_WITH_INFO:
				with_info = TRUE;
				SET_ROW_DIAG(rowno, i + 1);
			case SQL_SUCCESS:
				break;
			default: /* error */
				SET_ROW_DIAG(rowno, i + 1);
				return ret;
		}
	}

	if (ird->array_status_ptr) {
		ird->array_status_ptr[pos] = with_info ? SQL_ROW_SUCCESS_WITH_INFO :
			SQL_ROW_SUCCESS;
		DBGH(stmt, "status array @0x%p#%d set to %d.", ird->array_status_ptr,
			pos, ird->array_status_ptr[pos]);
	}

	return with_info ? SQL_SUCCESS_WITH_INFO : SQL_SUCCESS;

#undef RET_ROW_DIAG
#undef SET_ROW_DIAG
}

static BOOL conv_supported(SQLSMALLINT sqltype, SQLSMALLINT ctype)
{
	switch (ctype) {
		case SQL_C_GUID:

		case SQL_C_INTERVAL_DAY:
		case SQL_C_INTERVAL_HOUR:
		case SQL_C_INTERVAL_MINUTE:
		case SQL_C_INTERVAL_SECOND:
		case SQL_C_INTERVAL_DAY_TO_HOUR:
		case SQL_C_INTERVAL_DAY_TO_MINUTE:
		case SQL_C_INTERVAL_DAY_TO_SECOND:
		case SQL_C_INTERVAL_HOUR_TO_MINUTE:
		case SQL_C_INTERVAL_HOUR_TO_SECOND:
		case SQL_C_INTERVAL_MINUTE_TO_SECOND:
		case SQL_C_INTERVAL_MONTH:
		case SQL_C_INTERVAL_YEAR:
		case SQL_C_INTERVAL_YEAR_TO_MONTH:

			// case SQL_C_TYPE_TIMESTAMP_WITH_TIMEZONE:
			// case SQL_C_TYPE_TIME_WITH_TIMEZONE:
			return FALSE;
	}
	return TRUE;
}

/* implements the rotation of the matrix in:
 * https://docs.microsoft.com/en-us/sql/odbc/reference/appendixes/converting-data-from-c-to-sql-data-types */
static BOOL conv_allowed(SQLSMALLINT sqltype, SQLSMALLINT ctype)
{
	switch (ctype) {
		/* application will use implementation's type (irec's) */
		case SQL_C_DEFAULT:
		/* anything's convertible to [w]char & binary */
		case SQL_C_CHAR:
		case SQL_C_WCHAR:
		case SQL_C_BINARY:
			return TRUE;

		/* GUID (outlier) is only convertible to same time in both sets */
		case SQL_C_GUID:
			return sqltype == SQL_GUID;
	}

	switch (sqltype) {
		case SQL_CHAR:
		case SQL_VARCHAR:
		case SQL_LONGVARCHAR:
		case SQL_WCHAR:
		case SQL_WVARCHAR:
		case SQL_WLONGVARCHAR:
			break; /* it's not SQL_C_GUID, checked for above */

		case SQL_DECIMAL:
		case SQL_NUMERIC:
		case SQL_SMALLINT:
		case SQL_INTEGER:
		case SQL_TINYINT:
		case SQL_BIGINT:
			switch (ctype) {
				case SQL_C_TYPE_DATE:
				case SQL_C_TYPE_TIME:
				case SQL_C_TYPE_TIMESTAMP:
				case SQL_C_GUID:
					return FALSE;
			}
			break;

		case SQL_BIT:
		case SQL_REAL:
		case SQL_FLOAT:
		case SQL_DOUBLE:
			switch (ctype) {
				case SQL_C_TYPE_DATE:
				case SQL_C_TYPE_TIME:
				case SQL_C_TYPE_TIMESTAMP:
				case SQL_C_GUID:

				case SQL_C_INTERVAL_MONTH:
				case SQL_C_INTERVAL_YEAR:
				case SQL_C_INTERVAL_YEAR_TO_MONTH:
				case SQL_C_INTERVAL_DAY:
				case SQL_C_INTERVAL_HOUR:
				case SQL_C_INTERVAL_MINUTE:
				case SQL_C_INTERVAL_SECOND:
				case SQL_C_INTERVAL_DAY_TO_HOUR:
				case SQL_C_INTERVAL_DAY_TO_MINUTE:
				case SQL_C_INTERVAL_DAY_TO_SECOND:
				case SQL_C_INTERVAL_HOUR_TO_MINUTE:
				case SQL_C_INTERVAL_HOUR_TO_SECOND:
				case SQL_C_INTERVAL_MINUTE_TO_SECOND:
					return FALSE;
			}
			break;

		case SQL_BINARY:
		case SQL_VARBINARY:
		case SQL_LONGVARBINARY:
			return FALSE; /* it's not SQL_C_BINARY, checked for above */


		case SQL_TYPE_DATE:
			switch (ctype) {
				case SQL_C_TYPE_DATE:
				case SQL_C_TYPE_TIMESTAMP:
					return TRUE;
			}
			return FALSE;
		case SQL_TYPE_TIME:
			switch (ctype) {
				case SQL_C_TYPE_TIME:
				case SQL_C_TYPE_TIMESTAMP:
					return TRUE;
			}
			return FALSE;
		case SQL_TYPE_TIMESTAMP:
			switch (ctype) {
				case SQL_C_TYPE_DATE:
				case SQL_C_TYPE_TIME:
				case SQL_C_TYPE_TIMESTAMP:
					return TRUE;
			}
			return FALSE;

		case SQL_INTERVAL_MONTH:
		case SQL_INTERVAL_YEAR:
		case SQL_INTERVAL_YEAR_TO_MONTH:
		case SQL_INTERVAL_DAY:
		case SQL_INTERVAL_HOUR:
		case SQL_INTERVAL_MINUTE:
		case SQL_INTERVAL_SECOND:
		case SQL_INTERVAL_DAY_TO_HOUR:
		case SQL_INTERVAL_DAY_TO_MINUTE:
		case SQL_INTERVAL_DAY_TO_SECOND:
		case SQL_INTERVAL_HOUR_TO_MINUTE:
		case SQL_INTERVAL_HOUR_TO_SECOND:
		case SQL_INTERVAL_MINUTE_TO_SECOND:
			switch (ctype) {
				case SQL_C_BIT:
				case SQL_C_TINYINT:
				case SQL_C_STINYINT:
				case SQL_C_UTINYINT:
				case SQL_C_SBIGINT:
				case SQL_C_UBIGINT:
				case SQL_C_SHORT:
				case SQL_C_SSHORT:
				case SQL_C_USHORT:
				case SQL_C_LONG:
				case SQL_C_SLONG:
				case SQL_C_ULONG:
					return TRUE;
			}
			return FALSE;

	}
	return TRUE;
}

/* check if data types in returned columns are compabile with buffer types
 * bound for those columns */
static int sql2c_convertible(esodbc_stmt_st *stmt)
{
	SQLSMALLINT i, min;
	esodbc_desc_st *ard, *ird;
	esodbc_rec_st *arec, *irec;

	assert(stmt->hdr.dbc->es_types);
	assert(STMT_HAS_RESULTSET(stmt));

	ard = stmt->ard;
	ird = stmt->ird;

	min = ard->count < ird->count ? ard->count : ird->count;
	for (i = 0; i < min; i ++) {
		arec = &ard->recs[i];
		if (! REC_IS_BOUND(arec)) {
			/* skip not bound columns */
			continue;
		}
		irec = &ird->recs[i];

		if (! conv_allowed(irec->concise_type, arec->type)) {
			ERRH(stmt, "conversion not allowed on column %d types: IRD: %d, "
				"ARD: %d.", i, irec->es_type->c_concise_type, arec->type);
			return CONVERSION_VIOLATION;
		}
		if (! conv_supported(irec->concise_type, arec->type)) {
			ERRH(stmt, "conversion not supported on column %d types: IRD: %d, "
				"ARD: %d.", i, irec->es_type->c_concise_type, arec->type);
			return CONVERSION_UNSUPPORTED;
		}
	}

	return CONVERSION_SUPPORTED;
}

/*
 * "SQLFetch and SQLFetchScroll use the rowset size at the time of the call to
 * determine how many rows to fetch."
 *
 * "If SQLFetch or SQLFetchScroll encounters an error while retrieving one row
 * of a multirow rowset, or if SQLBulkOperations with an Operation argument of
 * SQL_FETCH_BY_BOOKMARK encounters an error while performing a bulk fetch, it
 * sets the corresponding value in the row status array to SQL_ROW_ERROR,
 * continues fetching rows, and returns SQL_SUCCESS_WITH_INFO."
 *
 * "SQLFetch can be used only for multirow fetches when called in ODBC 3.x; if
 * an ODBC 2.x application calls SQLFetch, it will open only a single-row,
 * forward-only cursor."
 *
 * "The application can change the rowset size and bind new rowset buffers (by
 * calling SQLBindCol or specifying a bind offset) even after rows have been
 * fetched."
 *
 * "SQLFetch returns bookmarks if column 0 is bound." Otherwise, "return more
 * than one row" (if avail).
 *
 * "The driver does not return SQLSTATE 01S01 (Error in row) to indicate that
 * an error has occurred while rows were fetched by a call to SQLFetch." (same
 * for SQLFetchScroll).
 *
 * "SQL_ROW_NOROW: The rowset overlapped the end of the result set, and no row
 * was returned that corresponded to this element of the row status array."
 *
 * "If the bound address is 0, no data value is returned" (also for row/column
 * binding)
 *
 * "In the IRD, this header field points to a row status array containing
 * status values after a call to SQLBulkOperations, SQLFetch, SQLFetchScroll,
 * or SQLSetPos."  = row status array of IRD (.array_status_ptr); can be NULL.
 *
 * "The binding offset is always added directly to the values in the
 * SQL_DESC_DATA_PTR, SQL_DESC_INDICATOR_PTR, and SQL_DESC_OCTET_LENGTH_PTR
 * fields." (.bind_offset.ptr)
 *
 * "In ARDs, this field specifies the binding orientation when SQLFetchScroll
 * or SQLFetch is called on the associated statement handle." (.bind_type)
 *
 * "In an IRD, this SQLULEN * header field points to a buffer containing the
 * number of rows fetched after a call to SQLFetch or SQLFetchScroll, or the
 * number of rows affected in a bulk operation performed by a call to
 * SQLBulkOperations or SQLSetPos, including error rows."
 * (.rows_processed_ptr)
 *
 * "The variable that the StrLen_or_Ind argument refers to is used for both
 * indicator and length information. If a fetch encounters a null value for
 * the column, it stores SQL_NULL_DATA in this variable; otherwise, it stores
 * the data length in this variable. Passing a null pointer as StrLen_or_Ind
 * keeps the fetch operation from returning the data length but makes the
 * fetch fail if it encounters a null value and has no way to return
 * SQL_NULL_DATA." (.indicator_ptr)
 */
SQLRETURN EsSQLFetch(SQLHSTMT StatementHandle)
{
	esodbc_stmt_st *stmt;
	esodbc_desc_st *ard, *ird;
	SQLULEN i, j;
	UJObject row;
	SQLRETURN ret;
	int errors;

	stmt = STMH(StatementHandle);
	ard = stmt->ard;
	ird = stmt->ird;

	if (! STMT_HAS_RESULTSET(stmt)) {
		if (STMT_NODATA_FORCED(stmt)) {
			DBGH(stmt, "empty result set flag set - returning no data.");
			return SQL_NO_DATA;
		}
		ERRH(stmt, "no resultset available on statement.");
		RET_HDIAGS(stmt, SQL_STATE_HY010);
	}

	/* Check if the data [type] stored in DB is compatiblie with the buffer
	 * [type] the application provides. This test can only be done at
	 * fetch-time, since the application can unbind/rebind columns at any time
	 * (i.e. also in-between consecutive fetches). */
	switch (stmt->sql2c_conversion) {
		case CONVERSION_VIOLATION:
			ERRH(stmt, "types compibility check had failed already "
				"(violation).");
			RET_HDIAGS(stmt, SQL_STATE_07006);
		/* RET_ returns */

		case CONVERSION_UNSUPPORTED:
			ERRH(stmt, "types compibility check had failed already "
				"(unsupported).");
			RET_HDIAG(stmt, SQL_STATE_HYC00, "Conversion target type not"
				" supported", 0);
		/* RET_ returns */

		case CONVERSION_SKIPPED:
			DBGH(stmt, "types compatibility skipped.");
			/* check unnecessary (SYS TYPES, possiblity other metas) */
			break;

		case CONVERSION_UNCHECKED:
			stmt->sql2c_conversion = sql2c_convertible(stmt);
			if (stmt->sql2c_conversion < 0) {
				ERRH(stmt, "convertibility check: failed!");
				RET_HDIAGS(stmt,
					stmt->sql2c_conversion == CONVERSION_VIOLATION ?
					SQL_STATE_07006 : SQL_STATE_HYC00);
			}
			DBGH(stmt, "convertibility check: OK.");
		/* no break; */

		default:
			DBGH(stmt, "ES/app data/buffer types found compatible.");
	}

	DBGH(stmt, "(`%.*s`); cursor @ %zd / %zd.", stmt->sqllen,
		stmt->u8sql, stmt->rset.vrows, stmt->rset.nrows);

	DBGH(stmt, "rowset max size: %d.", ard->array_size);
	errors = 0;
	/* for all rows in rowset/array, iterate over rows in current resultset */
	for (i = stmt->rset.array_pos; i < ard->array_size; i ++) {
		if (! UJIterArray(&stmt->rset.rows_iter, &row)) {
			DBGH(stmt, "ran out of rows in current result set: nrows=%zd, "
				"vrows=%zd.", stmt->rset.nrows, stmt->rset.vrows);
			if (stmt->rset.eccnt) { /*do I have an Elastic cursor? */
				stmt->rset.array_pos = i;
				ret = post_statement(stmt);
				if (! SQL_SUCCEEDED(ret)) {
					ERRH(stmt, "failed to fetch next results.");
					return ret;
				}
				return EsSQLFetch(StatementHandle);
			} else {
				DBGH(stmt, "reached end of entire result set. fetched=%zd.",
					stmt->rset.frows);
				/* indicate the non-processed rows in rowset */
				if (ard->array_status_ptr)
					for (j = i; j < ard->array_size; j ++) {
						ard->array_status_ptr[j] = SQL_ROW_NOROW;
					}
			}
			break;
		}
		ret = copy_one_row(stmt, i, row);
		if (! SQL_SUCCEEDED(ret)) {
			ERRH(stmt, "copying row %zd failed.", stmt->rset.vrows + i + 1);
			errors ++;
		}
	}
	stmt->rset.array_pos = 0;

	/* account for processed rows */
	stmt->rset.vrows += i;
	stmt->rset.frows += i;

	/* return number of processed rows (even if 0) */
	if (ird->rows_processed_ptr) {
		DBGH(stmt, "setting number of processed rows to: %u.", i);
		*ird->rows_processed_ptr = i;
	}

	if (i <= 0) {
		DBGH(stmt, "no data %sto return.", stmt->rset.vrows ? "left ": "");
		return SQL_NO_DATA;
	}

	if (errors && i <= errors) {
		ERRH(stmt, "processing failed for all rows [%d].", errors);
		return SQL_ERROR;
	}

	/* only failures need stmt.diag defer'ing */
	return SQL_SUCCESS;
}

/*
 * "SQLSetPos uses the rowset size that is in effect as of the preceding call
 * to SQLFetch or SQLFetchScroll, because SQLSetPos operates on a rowset that
 * has already been set. SQLSetPos also will pick up the new rowset size if
 * SQLBulkOperations has been called after the rowset size was changed."
 *
 * "When a block cursor first returns a rowset, the current row is the first
 * row of the rowset. To change the current row, the application calls
 * SQLSetPos or SQLBulkOperations (to update by bookmark)."
 *
 * "The driver returns SQLSTATE 01S01 (Error in row) only to indicate that an
 * error has occurred while rows were fetched by a call to SQLSetPos to
 * perform a bulk operation when the function is called in state S7." (not
 * supported currently, with RO operation)
 *
 * "In the IRD, this header field points to a row status array containing
 * status values after a call to SQLBulkOperations, SQLFetch, SQLFetchScroll,
 * or SQLSetPos."  = row status array of IRD (.array_status_ptr)
 *
 * "In the ARD, this header field points to a row operation array of values
 * that can be set by the application to indicate whether this row is to be
 * ignored for SQLSetPos operations." .array_status_ptr
 * "If the value in the SQL_DESC_ARRAY_STATUS_PTR field of the ARD is a null
 * pointer, all rows are included in the bulk operation"
 */
SQLRETURN EsSQLSetPos(
	SQLHSTMT        StatementHandle,
	SQLSETPOSIROW   RowNumber,
	SQLUSMALLINT    Operation,
	SQLUSMALLINT    LockType)
{
	switch(Operation) {
		case SQL_POSITION:
			// FIXME
			FIXME;
			break;

		case SQL_REFRESH:
		case SQL_UPDATE:
		case SQL_DELETE:
			ERRH(StatementHandle, "operation %d not supported.", Operation);
			RET_HDIAGS(STMH(StatementHandle), SQL_STATE_HYC00);
		default:
			ERRH(StatementHandle, "unknown operation type: %d.", Operation);
			RET_HDIAGS(STMH(StatementHandle), SQL_STATE_HY092);
	}
	return SQL_SUCCESS;
}

/*
 * == JDBC's Jdbc/PreparedStatement.executeLargeUpdate()
 * "SQLBulkOperations uses the rowset size in effect at the time of the call,
 * because it performs operations on a table independent of any fetched
 * rowset."
 * "In the IRD, this header field points to a row status array containing
 * status values after a call to SQLBulkOperations, SQLFetch, SQLFetchScroll,
 * or SQLSetPos."  = row status array of IRD (.array_status_ptr)
 */
SQLRETURN EsSQLBulkOperations(
	SQLHSTMT            StatementHandle,
	SQLSMALLINT         Operation)
{
	ERRH(StatementHandle, "data update functions not supported");
	RET_HDIAGS(STMH(StatementHandle), SQL_STATE_IM001);
}

SQLRETURN EsSQLCloseCursor(SQLHSTMT StatementHandle)
{
	esodbc_stmt_st *stmt = STMH(StatementHandle);
	if (! STMT_HAS_RESULTSET(stmt)) {
		ERRH(stmt, "no open cursor for statement");
		RET_HDIAGS(stmt, SQL_STATE_24000);
	}
	return EsSQLFreeStmt(StatementHandle, SQL_CLOSE);
}

SQLRETURN EsSQLNumResultCols(SQLHSTMT StatementHandle,
	_Out_ SQLSMALLINT *ColumnCount)
{
	return EsSQLGetDescFieldW(STMH(StatementHandle)->ird, NO_REC_NR,
			SQL_DESC_COUNT, ColumnCount, SQL_IS_SMALLINT, NULL);
}

/*
 * "The prepared statement associated with the statement handle can be
 * re-executed by calling SQLExecute until the application frees the statement
 * with a call to SQLFreeStmt with the SQL_DROP option or until the statement
 * handle is used in a call to SQLPrepare, SQLExecDirect, or one of the
 * catalog functions (SQLColumns, SQLTables, and so on)."
 */
SQLRETURN EsSQLPrepareW
(
	SQLHSTMT    hstmt,
	_In_reads_(cchSqlStr) SQLWCHAR *szSqlStr,
	SQLINTEGER  cchSqlStr
)
{
	esodbc_stmt_st *stmt = STMH(hstmt);
	SQLRETURN ret;

	if (cchSqlStr == SQL_NTS) {
		cchSqlStr = (SQLINTEGER)wcslen(szSqlStr);
	} else if (cchSqlStr <= 0) {
		ERRH(stmt, "invalid statment length: %d.", cchSqlStr);
		RET_HDIAGS(stmt, SQL_STATE_HY090);
	}
	DBGH(stmt, "preparing `" LWPDL "` [%d]", cchSqlStr, szSqlStr,
		cchSqlStr);

	ret = EsSQLFreeStmt(stmt, ESODBC_SQL_CLOSE);
	assert(SQL_SUCCEEDED(ret)); /* can't return error */

	return attach_sql(stmt, szSqlStr, cchSqlStr);
}


/*
 * "In the IPD, this header field points to a parameter status array
 * containing status information for each set of parameter values after a call
 * to SQLExecute or SQLExecDirect." = .array_status_ptr
 *
 * "In the APD, this header field points to a parameter operation array of
 * values that can be set by the application to indicate whether this set of
 * parameters is to be ignored when SQLExecute or SQLExecDirect is called."
 * = .array_status_ptr
 * "If no elements of the array are set, all sets of parameters in the array
 * are used in the SQLExecute or SQLExecDirect calls."
 */
SQLRETURN EsSQLExecute(SQLHSTMT hstmt)
{
	esodbc_stmt_st *stmt = STMH(hstmt);

	DBGH(stmt, "executing `%.*s` (%zd)", stmt->sqllen, stmt->u8sql,
		stmt->sqllen);

	return post_statement(stmt);
}

/*
 * "In the IPD, this header field points to a parameter status array
 * containing status information for each set of parameter values after a call
 * to SQLExecute or SQLExecDirect." = .array_status_ptr
 *
 * "In the APD, this header field points to a parameter operation array of
 * values that can be set by the application to indicate whether this set of
 * parameters is to be ignored when SQLExecute or SQLExecDirect is called."
 * = .array_status_ptr
 * "If no elements of the array are set, all sets of parameters in the array
 * are used in the SQLExecute or SQLExecDirect calls."
 */
SQLRETURN EsSQLExecDirectW
(
	SQLHSTMT    hstmt,
	_In_reads_opt_(TextLength) SQLWCHAR *szSqlStr,
	SQLINTEGER cchSqlStr
)
{
	esodbc_stmt_st *stmt = STMH(hstmt);
	SQLRETURN ret;

	if (cchSqlStr == SQL_NTS) {
		cchSqlStr = (SQLINTEGER)wcslen(szSqlStr);
	} else if (cchSqlStr <= 0) {
		ERRH(stmt, "invalid statment length: %d.", cchSqlStr);
		RET_HDIAGS(stmt, SQL_STATE_HY090);
	}
	DBGH(stmt, "directly executing SQL: `" LWPDL "` [%d].", cchSqlStr,
		szSqlStr, cchSqlStr);

	ret = EsSQLFreeStmt(stmt, ESODBC_SQL_CLOSE);
	assert(SQL_SUCCEEDED(ret)); /* can't return error */

	if (1 < stmt->apd->array_size) { // & param marker is set!
		FIXME;    //FIXME: multiple executions?
	}

	ret = attach_sql(stmt, szSqlStr, cchSqlStr);
	if (SQL_SUCCEEDED(ret)) {
		ret = post_statement(stmt);
	}
#ifndef NDEBUG
	/* no reason to keep it (it can't be re-executed), except for debugging */
	detach_sql(stmt);
#endif /* NDEBUG */
	return ret;
}

static inline SQLULEN get_col_size(esodbc_rec_st *rec)
{
	assert(rec->desc->type == DESC_TYPE_IRD);

	switch (rec->meta_type) {
		case METATYPE_EXACT_NUMERIC:
		case METATYPE_FLOAT_NUMERIC:
			return rec->es_type->column_size;

		case METATYPE_STRING:
		case METATYPE_BIN:
		case METATYPE_DATETIME:
		case METATYPE_INTERVAL_WSEC:
		case METATYPE_INTERVAL_WOSEC:
		case METATYPE_BIT:
			return rec->length;

		case METATYPE_UID:
			BUGH(rec->desc, "unsupported data c-type: %d.", rec->concise_type);
	}
	/*
	 * https://docs.microsoft.com/en-us/sql/odbc/reference/syntax/sqldescribecol-function#arguments :
	 * "If the column size cannot be determined, the driver returns 0." */
	return 0;
}

static inline SQLSMALLINT get_col_decdigits(esodbc_rec_st *rec)
{
	assert(rec->desc->type == DESC_TYPE_IRD);
	switch (rec->meta_type) {
		case METATYPE_DATETIME:
		case METATYPE_INTERVAL_WSEC:
			/* TODO: pending GH#30002 actually */
			return 3;

		case METATYPE_EXACT_NUMERIC:
			return rec->es_type->maximum_scale;
	}
	/* 0 to be returned for unknown case:
	 * https://docs.microsoft.com/en-us/sql/odbc/reference/syntax/sqldescribecol-function#syntax
	 */
	return 0;
}

SQLRETURN EsSQLDescribeColW(
	SQLHSTMT            hstmt,
	SQLUSMALLINT        icol,
	_Out_writes_opt_(cchColNameMax)
	SQLWCHAR            *szColName,
	SQLSMALLINT         cchColNameMax,
	_Out_opt_
	SQLSMALLINT        *pcchColName,
	_Out_opt_
	SQLSMALLINT        *pfSqlType,
	_Out_opt_
	SQLULEN            *pcbColDef,
	_Out_opt_
	SQLSMALLINT        *pibScale,
	_Out_opt_
	SQLSMALLINT        *pfNullable)
{
	esodbc_stmt_st *stmt = STMH(hstmt);
	esodbc_rec_st *rec;
	SQLRETURN ret;
	SQLSMALLINT col_blen;

	DBGH(stmt, "IRD@0x%p, column #%d.", stmt->ird, icol);

	if (! STMT_HAS_RESULTSET(stmt)) {
		ERRH(stmt, "no resultset available on statement.");
		RET_HDIAGS(stmt, SQL_STATE_HY010);
	}

	if (icol < 1) {
		/* TODO: if implementing bookmarks */
		RET_HDIAGS(stmt, SQL_STATE_HYC00);
	}

	rec = get_record(stmt->ird, icol, FALSE);
	if (! rec) {
		ERRH(stmt, "no record for columns #%d.", icol);
		RET_HDIAGS(stmt, SQL_STATE_07009);
	}
#ifndef NDEBUG
	//dump_record(rec);
#endif /* NDEBUG */

	if (szColName) {
		ret = write_wstr(stmt, szColName, &rec->name,
				cchColNameMax * sizeof(*szColName), &col_blen);
		if (! SQL_SUCCEEDED(ret)) {
			ERRH(stmt, "failed to copy column name `" LWPDL "`.",
				LWSTR(&rec->name));
			return ret;
		}
	} else {
		DBGH(stmt, "NULL column name buffer provided.");
		col_blen = -1;
	}

	if (! pcchColName) {
		ERRH(stmt, "no column name length buffer provided.");
		RET_HDIAG(stmt, SQL_STATE_HY090,
			"no column name length buffer provided", 0);
	}
	*pcchColName = 0 <= col_blen ? (col_blen / sizeof(*szColName)) :
		(SQLSMALLINT)rec->name.cnt;
	DBGH(stmt, "col #%d name has %d chars.", icol, *pcchColName);

	if (! pfSqlType) {
		ERRH(stmt, "no column data type buffer provided.");
		RET_HDIAG(stmt, SQL_STATE_HY090,
			"no column data type buffer provided", 0);
	}
	*pfSqlType = rec->concise_type;
	DBGH(stmt, "col #%d has concise type=%d.", icol, *pfSqlType);

	if (! pcbColDef) {
		ERRH(stmt, "no column size buffer provided.");
		RET_HDIAG(stmt, SQL_STATE_HY090, "no column size buffer provided", 0);
	}
	*pcbColDef = get_col_size(rec);
	DBGH(stmt, "col #%d of meta type %d has size=%llu.",
		icol, rec->meta_type, *pcbColDef);

	if (! pibScale) {
		ERRH(stmt, "no column decimal digits buffer provided.");
		RET_HDIAG(stmt, SQL_STATE_HY090,
			"no column decimal digits buffer provided", 0);
	}
	*pibScale = get_col_decdigits(rec);
	DBGH(stmt, "col #%d of meta type %d has decimal digits=%d.",
		icol, rec->meta_type, *pibScale);

	if (! pfNullable) {
		ERRH(stmt, "no column decimal digits buffer provided.");
		RET_HDIAG(stmt, SQL_STATE_HY090,
			"no column decimal digits buffer provided", 0);
	}
	ASSERT_IXD_HAS_ES_TYPE(rec);
	/* TODO: this would be available in SQLColumns resultset. */
	*pfNullable = rec->es_type->nullable;
	DBGH(stmt, "col #%d nullable=%d.", icol, *pfNullable);

	return SQL_SUCCESS;
}


SQLRETURN EsSQLColAttributeW(
	SQLHSTMT        hstmt,
	SQLUSMALLINT    iCol,
	SQLUSMALLINT    iField,
	_Out_writes_bytes_opt_(cbDescMax)
	SQLPOINTER      pCharAttr, /* [out] value, if string; can be NULL */
	SQLSMALLINT     cbDescMax, /* [in] byte len of pCharAttr */
	_Out_opt_
	SQLSMALLINT     *pcbCharAttr, /* [out] len written in pCharAttr (w/o \0 */
	_Out_opt_
#ifdef _WIN64
	SQLLEN          *pNumAttr /* [out] value, if numeric */
#else /* _WIN64 */
	SQLPOINTER      pNumAttr
#endif /* _WIN64 */
)
{
	esodbc_stmt_st *stmt = STMH(hstmt);
	esodbc_desc_st *ird = stmt->ird;
	esodbc_rec_st *rec;
	SQLSMALLINT sint;
	wstr_st *wstrp;
	SQLLEN len;
	SQLINTEGER iint;

#ifdef _WIN64
#define PNUMATTR_ASSIGN(type, value) *pNumAttr = (SQLLEN)(value)
#else /* _WIN64 */
#define PNUMATTR_ASSIGN(type, value) *(type *)pNumAttr = (type)(value)
#endif /* _WIN64 */

	DBGH(stmt, "IRD@0x%p, column #%d, field: %d.", ird, iCol, iField);

	if (! STMT_HAS_RESULTSET(stmt)) {
		ERRH(stmt, "no resultset available on statement.");
		RET_HDIAGS(stmt, SQL_STATE_HY010);
	}

	if (iCol < 1) {
		/* TODO: if implementing bookmarks */
		RET_HDIAGS(stmt, SQL_STATE_HYC00);
	}

	rec = get_record(ird, iCol, FALSE);
	if (! rec) {
		ERRH(stmt, "no record for columns #%d.", iCol);
		RET_HDIAGS(stmt, SQL_STATE_07009);
	}

	ASSERT_IXD_HAS_ES_TYPE(rec);

	/*INDENT-OFF*/
	switch (iField) {
		/* SQLSMALLINT */
		do {
		case SQL_DESC_CONCISE_TYPE: sint = rec->concise_type; break;
		case SQL_DESC_TYPE: sint = rec->type; break;
		case SQL_DESC_UNNAMED: sint = rec->unnamed; break;
		case SQL_DESC_NULLABLE: sint = rec->es_type->nullable; break;
		case SQL_DESC_SEARCHABLE: sint = rec->es_type->searchable; break;
		case SQL_DESC_UNSIGNED: sint = rec->es_type->unsigned_attribute; break;
		case SQL_DESC_UPDATABLE: sint = rec->updatable; break;
		case SQL_DESC_PRECISION:
			sint = rec->es_type->fixed_prec_scale;
			break;
		case SQL_DESC_SCALE:
			sint = rec->es_type->maximum_scale;
			break;
		case SQL_DESC_FIXED_PREC_SCALE:
			sint = rec->es_type->fixed_prec_scale;
			break;
		} while (0);
			PNUMATTR_ASSIGN(SQLSMALLINT, sint);
			break;

		/* SQLWCHAR* */
		do {
		case SQL_DESC_BASE_COLUMN_NAME: wstrp = &rec->base_column_name; break;
		case SQL_DESC_LABEL: wstrp = &rec->label; break;
		case SQL_DESC_BASE_TABLE_NAME: wstrp = &rec->base_table_name; break;
		case SQL_DESC_CATALOG_NAME: wstrp = &rec->catalog_name; break;
		case SQL_DESC_NAME: wstrp = &rec->name; break;
		case SQL_DESC_SCHEMA_NAME: wstrp = &rec->schema_name; break;
		case SQL_DESC_TABLE_NAME: wstrp = &rec->table_name; break;
		case SQL_DESC_LITERAL_PREFIX:
			wstrp = &rec->es_type->literal_prefix;
			break;
		case SQL_DESC_LITERAL_SUFFIX:
			wstrp = &rec->es_type->literal_suffix;
			break;
		case SQL_DESC_LOCAL_TYPE_NAME:
			wstrp = &rec->es_type->type_name;
			break;
		case SQL_DESC_TYPE_NAME:
			wstrp = &rec->es_type->type_name;
			break;
		} while (0);
			return write_wstr(stmt, pCharAttr, wstrp, cbDescMax, pcbCharAttr);

		/* SQLLEN */
		do {
		case SQL_DESC_DISPLAY_SIZE: len = rec->es_type->display_size; break;
		case SQL_DESC_OCTET_LENGTH: len = rec->octet_length; break;
		} while (0);
			PNUMATTR_ASSIGN(SQLLEN, len);
			break;

		/* SQLULEN */
		case SQL_DESC_LENGTH:
			/* "This information is returned from the SQL_DESC_LENGTH record
			 * field of the IRD." */
			PNUMATTR_ASSIGN(SQLULEN, rec->length);
			break;

		/* SQLINTEGER */
		do {
		case SQL_DESC_AUTO_UNIQUE_VALUE:
			iint = rec->es_type->auto_unique_value;
			break;
		case SQL_DESC_CASE_SENSITIVE:
			iint = rec->es_type->case_sensitive;
			break;
		case SQL_DESC_NUM_PREC_RADIX: iint = rec->num_prec_radix; break;
		} while (0);
			PNUMATTR_ASSIGN(SQLINTEGER, iint);
			break;


		case SQL_DESC_COUNT:
			PNUMATTR_ASSIGN(SQLSMALLINT, ird->count);
			break;

		default:
			ERRH(stmt, "unknown field type %d.", iField);
			RET_HDIAGS(stmt, SQL_STATE_HY091);
	}
	/*INDENT-ON*/

	return SQL_SUCCESS;
#undef PNUMATTR_ASSIGN
}

SQLRETURN EsSQLRowCount(_In_ SQLHSTMT StatementHandle, _Out_ SQLLEN *RowCount)
{
	esodbc_stmt_st *stmt = STMH(StatementHandle);

	if (! STMT_HAS_RESULTSET(stmt)) {
		ERRH(stmt, "no resultset available on statement.");
		RET_HDIAGS(stmt, SQL_STATE_HY010);
	}

	DBGH(stmt, "current resultset rows count: %zd.", stmt->rset.nrows);
	*RowCount = (SQLLEN)stmt->rset.nrows;

	if (stmt->rset.eccnt) {
		/* fetch_size or scroller size chunks the result */
		WARNH(stmt, "this function will only return the row count of the "
			"partial result set available.");
		/* returning a _WITH_INFO here will fail the query for MSQRY32.. */
		//RET_HDIAG(stmt, SQL_STATE_01000, "row count is for partial result "
		//		"only", 0);
	}
	return SQL_SUCCESS;
}

/* vim: set noet fenc=utf-8 ff=dos sts=0 sw=4 ts=4 : */
