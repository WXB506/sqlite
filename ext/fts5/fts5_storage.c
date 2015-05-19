/*
** 2014 May 31
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
******************************************************************************
**
*/

#ifdef SQLITE_ENABLE_FTS5


#include "fts5Int.h"

struct Fts5Storage {
  Fts5Config *pConfig;
  Fts5Index *pIndex;
  int bTotalsValid;               /* True if nTotalRow/aTotalSize[] are valid */
  i64 nTotalRow;                  /* Total number of rows in FTS table */
  i64 *aTotalSize;                /* Total sizes of each column */ 
  sqlite3_stmt *aStmt[10];
};


#if FTS5_STMT_SCAN_ASC!=0 
# error "FTS5_STMT_SCAN_ASC mismatch" 
#endif
#if FTS5_STMT_SCAN_DESC!=1 
# error "FTS5_STMT_SCAN_DESC mismatch" 
#endif
#if FTS5_STMT_LOOKUP!=2
# error "FTS5_STMT_LOOKUP mismatch" 
#endif

#define FTS5_STMT_INSERT_CONTENT  3
#define FTS5_STMT_REPLACE_CONTENT 4

#define FTS5_STMT_DELETE_CONTENT  5
#define FTS5_STMT_REPLACE_DOCSIZE  6
#define FTS5_STMT_DELETE_DOCSIZE  7

#define FTS5_STMT_LOOKUP_DOCSIZE  8

#define FTS5_STMT_REPLACE_CONFIG 9

/*
** Prepare the two insert statements - Fts5Storage.pInsertContent and
** Fts5Storage.pInsertDocsize - if they have not already been prepared.
** Return SQLITE_OK if successful, or an SQLite error code if an error
** occurs.
*/
static int fts5StorageGetStmt(
  Fts5Storage *p,                 /* Storage handle */
  int eStmt,                      /* FTS5_STMT_XXX constant */
  sqlite3_stmt **ppStmt,          /* OUT: Prepared statement handle */
  char **pzErrMsg                 /* OUT: Error message (if any) */
){
  int rc = SQLITE_OK;

  assert( eStmt>=0 && eStmt<ArraySize(p->aStmt) );
  if( p->aStmt[eStmt]==0 ){
    const char *azStmt[] = {
      "SELECT %s FROM %s T ORDER BY T.%Q ASC",          /* SCAN_ASC */
      "SELECT %s FROM %s T ORDER BY T.%Q DESC",         /* SCAN_DESC */
      "SELECT %s FROM %s T WHERE    T.%Q=?",            /* LOOKUP  */

      "INSERT INTO %Q.'%q_content' VALUES(%s)",         /* INSERT_CONTENT  */
      "REPLACE INTO %Q.'%q_content' VALUES(%s)",        /* REPLACE_CONTENT */
      "DELETE FROM %Q.'%q_content' WHERE id=?",         /* DELETE_CONTENT  */
      "REPLACE INTO %Q.'%q_docsize' VALUES(?,?)",       /* REPLACE_DOCSIZE  */
      "DELETE FROM %Q.'%q_docsize' WHERE id=?",         /* DELETE_DOCSIZE  */

      "SELECT sz FROM %Q.'%q_docsize' WHERE id=?",      /* LOOKUP_DOCSIZE  */

      "REPLACE INTO %Q.'%q_config' VALUES(?,?)",        /* REPLACE_CONFIG */
    };
    Fts5Config *pC = p->pConfig;
    char *zSql = 0;

    switch( eStmt ){
      case FTS5_STMT_SCAN_ASC:
      case FTS5_STMT_SCAN_DESC:
      case FTS5_STMT_LOOKUP:
        zSql = sqlite3_mprintf(azStmt[eStmt], 
            pC->zContentExprlist, pC->zContent, pC->zContentRowid
        );
        break;

      case FTS5_STMT_INSERT_CONTENT: 
      case FTS5_STMT_REPLACE_CONTENT: {
        int nCol = pC->nCol + 1;
        char *zBind;
        int i;

        zBind = sqlite3_malloc(1 + nCol*2);
        if( zBind ){
          for(i=0; i<nCol; i++){
            zBind[i*2] = '?';
            zBind[i*2 + 1] = ',';
          }
          zBind[i*2-1] = '\0';
          zSql = sqlite3_mprintf(azStmt[eStmt], pC->zDb, pC->zName, zBind);
          sqlite3_free(zBind);
        }
        break;
      }

      default:
        zSql = sqlite3_mprintf(azStmt[eStmt], pC->zDb, pC->zName);
        break;
    }

    if( zSql==0 ){
      rc = SQLITE_NOMEM;
    }else{
      rc = sqlite3_prepare_v2(pC->db, zSql, -1, &p->aStmt[eStmt], 0);
      sqlite3_free(zSql);
      if( rc!=SQLITE_OK && pzErrMsg ){
        *pzErrMsg = sqlite3_mprintf("%s", sqlite3_errmsg(pC->db));
      }
    }
  }

  *ppStmt = p->aStmt[eStmt];
  return rc;
}


static int fts5ExecPrintf(
  sqlite3 *db,
  char **pzErr,
  const char *zFormat,
  ...
){
  int rc;
  va_list ap;                     /* ... printf arguments */
  va_start(ap, zFormat);
  char *zSql = sqlite3_vmprintf(zFormat, ap);

  if( zSql==0 ){
    rc = SQLITE_NOMEM;
  }else{
    rc = sqlite3_exec(db, zSql, 0, 0, pzErr);
    sqlite3_free(zSql);
  }

  va_end(ap);
  return rc;
}

/*
** Drop all shadow tables. Return SQLITE_OK if successful or an SQLite error
** code otherwise.
*/
int sqlite3Fts5DropAll(Fts5Config *pConfig){
  int rc = fts5ExecPrintf(pConfig->db, 0, 
      "DROP TABLE IF EXISTS %Q.'%q_data';"
      "DROP TABLE IF EXISTS %Q.'%q_docsize';"
      "DROP TABLE IF EXISTS %Q.'%q_config';",
      pConfig->zDb, pConfig->zName,
      pConfig->zDb, pConfig->zName,
      pConfig->zDb, pConfig->zName
  );
  if( rc==SQLITE_OK && pConfig->eContent==FTS5_CONTENT_NORMAL ){
    rc = fts5ExecPrintf(pConfig->db, 0, 
        "DROP TABLE IF EXISTS %Q.'%q_content';",
        pConfig->zDb, pConfig->zName
    );
  }
  return rc;
}

/*
** Create the shadow table named zPost, with definition zDefn. Return
** SQLITE_OK if successful, or an SQLite error code otherwise.
*/
int sqlite3Fts5CreateTable(
  Fts5Config *pConfig,            /* FTS5 configuration */
  const char *zPost,              /* Shadow table to create (e.g. "content") */
  const char *zDefn,              /* Columns etc. for shadow table */
  int bWithout,                   /* True for without rowid */
  char **pzErr                    /* OUT: Error message */
){
  int rc;
  char *zErr = 0;

  rc = fts5ExecPrintf(pConfig->db, &zErr, "CREATE TABLE %Q.'%q_%q'(%s)%s",
      pConfig->zDb, pConfig->zName, zPost, zDefn, bWithout?" WITHOUT ROWID":""
  );
  if( zErr ){
    *pzErr = sqlite3_mprintf(
        "fts5: error creating shadow table %q_%s: %s", 
        pConfig->zName, zPost, zErr
    );
    sqlite3_free(zErr);
  }

  return rc;
}

/*
** Open a new Fts5Index handle. If the bCreate argument is true, create
** and initialize the underlying tables 
**
** If successful, set *pp to point to the new object and return SQLITE_OK.
** Otherwise, set *pp to NULL and return an SQLite error code.
*/
int sqlite3Fts5StorageOpen(
  Fts5Config *pConfig, 
  Fts5Index *pIndex, 
  int bCreate, 
  Fts5Storage **pp,
  char **pzErr                    /* OUT: Error message */
){
  int rc = SQLITE_OK;
  Fts5Storage *p;                 /* New object */
  int nByte;                      /* Bytes of space to allocate */

  nByte = sizeof(Fts5Storage)               /* Fts5Storage object */
        + pConfig->nCol * sizeof(i64);      /* Fts5Storage.aTotalSize[] */
  *pp = p = (Fts5Storage*)sqlite3_malloc(nByte);
  if( !p ) return SQLITE_NOMEM;

  memset(p, 0, nByte);
  p->aTotalSize = (i64*)&p[1];
  p->pConfig = pConfig;
  p->pIndex = pIndex;

  if( bCreate ){
    if( pConfig->eContent==FTS5_CONTENT_NORMAL ){
      char *zDefn = sqlite3_malloc(32 + pConfig->nCol * 10);
      if( zDefn==0 ){
        rc = SQLITE_NOMEM;
      }else{
        int i;
        int iOff = sprintf(zDefn, "id INTEGER PRIMARY KEY");
        for(i=0; i<pConfig->nCol; i++){
          iOff += sprintf(&zDefn[iOff], ", c%d", i);
        }
        rc = sqlite3Fts5CreateTable(pConfig, "content", zDefn, 0, pzErr);
      }
      sqlite3_free(zDefn);
    }

    if( rc==SQLITE_OK ){
      rc = sqlite3Fts5CreateTable(
          pConfig, "docsize", "id INTEGER PRIMARY KEY, sz BLOB", 0, pzErr
      );
    }
    if( rc==SQLITE_OK ){
      rc = sqlite3Fts5CreateTable(
          pConfig, "config", "k PRIMARY KEY, v", 1, pzErr
      );
    }
    if( rc==SQLITE_OK ){
      rc = sqlite3Fts5StorageConfigValue(p, "version", 0, FTS5_CURRENT_VERSION);
    }
  }

  if( rc ){
    sqlite3Fts5StorageClose(p);
    *pp = 0;
  }
  return rc;
}

/*
** Close a handle opened by an earlier call to sqlite3Fts5StorageOpen().
*/
int sqlite3Fts5StorageClose(Fts5Storage *p){
  int rc = SQLITE_OK;
  if( p ){
    int i;

    /* Finalize all SQL statements */
    for(i=0; i<ArraySize(p->aStmt); i++){
      sqlite3_finalize(p->aStmt[i]);
    }

    sqlite3_free(p);
  }
  return rc;
}

typedef struct Fts5InsertCtx Fts5InsertCtx;
struct Fts5InsertCtx {
  Fts5Storage *pStorage;
  int iCol;
  int szCol;                      /* Size of column value in tokens */
};

/*
** Tokenization callback used when inserting tokens into the FTS index.
*/
static int fts5StorageInsertCallback(
  void *pContext,                 /* Pointer to Fts5InsertCtx object */
  const char *pToken,             /* Buffer containing token */
  int nToken,                     /* Size of token in bytes */
  int iStart,                     /* Start offset of token */
  int iEnd                        /* End offset of token */
){
  Fts5InsertCtx *pCtx = (Fts5InsertCtx*)pContext;
  Fts5Index *pIdx = pCtx->pStorage->pIndex;
  int iPos = pCtx->szCol++;
  return sqlite3Fts5IndexWrite(pIdx, pCtx->iCol, iPos, pToken, nToken);
}

/*
** If a row with rowid iDel is present in the %_content table, add the
** delete-markers to the FTS index necessary to delete it. Do not actually
** remove the %_content row at this time though.
*/
static int fts5StorageDeleteFromIndex(Fts5Storage *p, i64 iDel){
  Fts5Config *pConfig = p->pConfig;
  sqlite3_stmt *pSeek;            /* SELECT to read row iDel from %_data */
  int rc;                         /* Return code */

  rc = fts5StorageGetStmt(p, FTS5_STMT_LOOKUP, &pSeek, 0);
  if( rc==SQLITE_OK ){
    int rc2;
    sqlite3_bind_int64(pSeek, 1, iDel);
    if( sqlite3_step(pSeek)==SQLITE_ROW ){
      int iCol;
      Fts5InsertCtx ctx;
      ctx.pStorage = p;
      ctx.iCol = -1;
      rc = sqlite3Fts5IndexBeginWrite(p->pIndex, iDel);
      for(iCol=1; rc==SQLITE_OK && iCol<=pConfig->nCol; iCol++){
        if( pConfig->abUnindexed[iCol-1] ) continue;
        ctx.szCol = 0;
        rc = sqlite3Fts5Tokenize(pConfig, 
            (const char*)sqlite3_column_text(pSeek, iCol),
            sqlite3_column_bytes(pSeek, iCol),
            (void*)&ctx,
            fts5StorageInsertCallback
        );
        p->aTotalSize[iCol-1] -= (i64)ctx.szCol;
      }
      p->nTotalRow--;
    }
    rc2 = sqlite3_reset(pSeek);
    if( rc==SQLITE_OK ) rc = rc2;
  }

  return rc;
}


/*
** Insert a record into the %_docsize table. Specifically, do:
**
**   INSERT OR REPLACE INTO %_docsize(id, sz) VALUES(iRowid, pBuf);
*/
static int fts5StorageInsertDocsize(
  Fts5Storage *p,                 /* Storage module to write to */
  i64 iRowid,                     /* id value */
  Fts5Buffer *pBuf                /* sz value */
){
  sqlite3_stmt *pReplace = 0;
  int rc = fts5StorageGetStmt(p, FTS5_STMT_REPLACE_DOCSIZE, &pReplace, 0);
  if( rc==SQLITE_OK ){
    sqlite3_bind_int64(pReplace, 1, iRowid);
    sqlite3_bind_blob(pReplace, 2, pBuf->p, pBuf->n, SQLITE_STATIC);
    sqlite3_step(pReplace);
    rc = sqlite3_reset(pReplace);
  }
  return rc;
}

/*
** Load the contents of the "averages" record from disk into the 
** p->nTotalRow and p->aTotalSize[] variables. If successful, and if
** argument bCache is true, set the p->bTotalsValid flag to indicate
** that the contents of aTotalSize[] and nTotalRow are valid until
** further notice.
**
** Return SQLITE_OK if successful, or an SQLite error code if an error
** occurs.
*/
static int fts5StorageLoadTotals(Fts5Storage *p, int bCache){
  int rc = SQLITE_OK;
  if( p->bTotalsValid==0 ){
    int nCol = p->pConfig->nCol;
    Fts5Buffer buf;
    memset(&buf, 0, sizeof(buf));

    memset(p->aTotalSize, 0, sizeof(i64) * nCol);
    p->nTotalRow = 0;
    rc = sqlite3Fts5IndexGetAverages(p->pIndex, &buf);
    if( rc==SQLITE_OK && buf.n ){
      int i = 0;
      int iCol;
      i += getVarint(&buf.p[i], (u64*)&p->nTotalRow);
      for(iCol=0; i<buf.n && iCol<nCol; iCol++){
        i += getVarint(&buf.p[i], (u64*)&p->aTotalSize[iCol]);
      }
    }
    sqlite3_free(buf.p);
    p->bTotalsValid = bCache;
  }
  return rc;
}

/*
** Store the current contents of the p->nTotalRow and p->aTotalSize[] 
** variables in the "averages" record on disk.
**
** Return SQLITE_OK if successful, or an SQLite error code if an error
** occurs.
*/
static int fts5StorageSaveTotals(Fts5Storage *p){
  int nCol = p->pConfig->nCol;
  int i;
  Fts5Buffer buf;
  int rc = SQLITE_OK;
  memset(&buf, 0, sizeof(buf));

  sqlite3Fts5BufferAppendVarint(&rc, &buf, p->nTotalRow);
  for(i=0; i<nCol; i++){
    sqlite3Fts5BufferAppendVarint(&rc, &buf, p->aTotalSize[i]);
  }
  if( rc==SQLITE_OK ){
    rc = sqlite3Fts5IndexSetAverages(p->pIndex, buf.p, buf.n);
  }
  sqlite3_free(buf.p);

  return rc;
}

/*
** Remove a row from the FTS table.
*/
int sqlite3Fts5StorageDelete(Fts5Storage *p, i64 iDel){
  int rc;
  sqlite3_stmt *pDel;

  rc = fts5StorageLoadTotals(p, 1);

  /* Delete the index records */
  if( rc==SQLITE_OK ){
    rc = fts5StorageDeleteFromIndex(p, iDel);
  }

  /* Delete the %_docsize record */
  if( rc==SQLITE_OK ){
    rc = fts5StorageGetStmt(p, FTS5_STMT_DELETE_DOCSIZE, &pDel, 0);
  }
  if( rc==SQLITE_OK ){
    sqlite3_bind_int64(pDel, 1, iDel);
    sqlite3_step(pDel);
    rc = sqlite3_reset(pDel);
  }

  /* Delete the %_content record */
  if( rc==SQLITE_OK ){
    rc = fts5StorageGetStmt(p, FTS5_STMT_DELETE_CONTENT, &pDel, 0);
  }
  if( rc==SQLITE_OK ){
    sqlite3_bind_int64(pDel, 1, iDel);
    sqlite3_step(pDel);
    rc = sqlite3_reset(pDel);
  }

  /* Write the averages record */
  if( rc==SQLITE_OK ){
    rc = fts5StorageSaveTotals(p);
  }

  return rc;
}

int sqlite3Fts5StorageSpecialDelete(
  Fts5Storage *p, 
  i64 iDel, 
  sqlite3_value **apVal
){
  Fts5Config *pConfig = p->pConfig;
  int rc;
  sqlite3_stmt *pDel;

  assert( pConfig->eContent!=FTS5_CONTENT_NORMAL );
  rc = fts5StorageLoadTotals(p, 1);

  /* Delete the index records */
  if( rc==SQLITE_OK ){
    int iCol;
    Fts5InsertCtx ctx;
    ctx.pStorage = p;
    ctx.iCol = -1;

    rc = sqlite3Fts5IndexBeginWrite(p->pIndex, iDel);
    for(iCol=0; rc==SQLITE_OK && iCol<pConfig->nCol; iCol++){
      if( pConfig->abUnindexed[iCol] ) continue;
      ctx.szCol = 0;
      rc = sqlite3Fts5Tokenize(pConfig, 
        (const char*)sqlite3_value_text(apVal[iCol]),
        sqlite3_value_bytes(apVal[iCol]),
        (void*)&ctx,
        fts5StorageInsertCallback
      );
      p->aTotalSize[iCol] -= (i64)ctx.szCol;
    }
    p->nTotalRow--;
  }

  /* Delete the %_docsize record */
  if( rc==SQLITE_OK ){
    rc = fts5StorageGetStmt(p, FTS5_STMT_DELETE_DOCSIZE, &pDel, 0);
  }
  if( rc==SQLITE_OK ){
    sqlite3_bind_int64(pDel, 1, iDel);
    sqlite3_step(pDel);
    rc = sqlite3_reset(pDel);
  }

  /* Write the averages record */
  if( rc==SQLITE_OK ){
    rc = fts5StorageSaveTotals(p);
  }

  return rc;
}

/*
** Delete all entries in the FTS5 index.
*/
int sqlite3Fts5StorageDeleteAll(Fts5Storage *p){
  Fts5Config *pConfig = p->pConfig;
  int rc;

  /* Delete the contents of the %_data and %_docsize tables. */
  rc = fts5ExecPrintf(pConfig->db, 0,
      "DELETE FROM %Q.'%q_data';"
      "DELETE FROM %Q.'%q_docsize';",
      pConfig->zDb, pConfig->zName,
      pConfig->zDb, pConfig->zName
  );

  /* Reinitialize the %_data table. This call creates the initial structure
  ** and averages records.  */
  if( rc==SQLITE_OK ){
    rc = sqlite3Fts5IndexReinit(p->pIndex);
  }
  if( rc==SQLITE_OK ){
    rc = sqlite3Fts5StorageConfigValue(p, "version", 0, FTS5_CURRENT_VERSION);
  }
  return rc;
}

int sqlite3Fts5StorageRebuild(Fts5Storage *p){
  Fts5Buffer buf = {0,0,0};
  Fts5Config *pConfig = p->pConfig;
  sqlite3_stmt *pScan = 0;
  Fts5InsertCtx ctx;
  int rc;

  memset(&ctx, 0, sizeof(Fts5InsertCtx));
  ctx.pStorage = p;
  rc = sqlite3Fts5StorageDeleteAll(p);
  if( rc==SQLITE_OK ){
    rc = fts5StorageLoadTotals(p, 1);
  }

  if( rc==SQLITE_OK ){
    rc = fts5StorageGetStmt(p, FTS5_STMT_SCAN_ASC, &pScan, 0);
  }

  while( rc==SQLITE_OK && SQLITE_ROW==sqlite3_step(pScan) ){
    i64 iRowid = sqlite3_column_int64(pScan, 0);

    sqlite3Fts5BufferZero(&buf);
    rc = sqlite3Fts5IndexBeginWrite(p->pIndex, iRowid);
    for(ctx.iCol=0; rc==SQLITE_OK && ctx.iCol<pConfig->nCol; ctx.iCol++){
      ctx.szCol = 0;
      if( pConfig->abUnindexed[ctx.iCol]==0 ){
        rc = sqlite3Fts5Tokenize(pConfig, 
            (const char*)sqlite3_column_text(pScan, ctx.iCol+1),
            sqlite3_column_bytes(pScan, ctx.iCol+1),
            (void*)&ctx,
            fts5StorageInsertCallback
        );
      }
      sqlite3Fts5BufferAppendVarint(&rc, &buf, ctx.szCol);
      p->aTotalSize[ctx.iCol] += (i64)ctx.szCol;
    }
    p->nTotalRow++;

    if( rc==SQLITE_OK ){
      rc = fts5StorageInsertDocsize(p, iRowid, &buf);
    }
  }
  sqlite3_free(buf.p);

  /* Write the averages record */
  if( rc==SQLITE_OK ){
    rc = fts5StorageSaveTotals(p);
  }
  return rc;
}

int sqlite3Fts5StorageOptimize(Fts5Storage *p){
  return sqlite3Fts5IndexOptimize(p->pIndex);
}

int sqlite3Fts5StorageMerge(Fts5Storage *p, int nMerge){
  return sqlite3Fts5IndexMerge(p->pIndex, nMerge);
}

/*
** Allocate a new rowid. This is used for "external content" tables when
** a NULL value is inserted into the rowid column. The new rowid is allocated
** by inserting a dummy row into the %_docsize table. The dummy will be
** overwritten later.
*/
static int fts5StorageNewRowid(Fts5Storage *p, i64 *piRowid){
  sqlite3_stmt *pReplace = 0;
  int rc = fts5StorageGetStmt(p, FTS5_STMT_REPLACE_DOCSIZE, &pReplace, 0);
  if( rc==SQLITE_OK ){
    sqlite3_bind_null(pReplace, 1);
    sqlite3_bind_null(pReplace, 2);
    sqlite3_step(pReplace);
    rc = sqlite3_reset(pReplace);
  }
  if( rc==SQLITE_OK ){
    *piRowid = sqlite3_last_insert_rowid(p->pConfig->db);
  }
  return rc;
}

/*
** Insert a new row into the FTS table.
*/
int sqlite3Fts5StorageInsert(
  Fts5Storage *p,                 /* Storage module to write to */
  sqlite3_value **apVal,          /* Array of values passed to xUpdate() */
  int eConflict,                  /* on conflict clause */
  i64 *piRowid                    /* OUT: rowid of new record */
){
  Fts5Config *pConfig = p->pConfig;
  int rc = SQLITE_OK;             /* Return code */
  sqlite3_stmt *pInsert;          /* Statement used to write %_content table */
  int eStmt = 0;                  /* Type of statement used on %_content */
  int i;                          /* Counter variable */
  Fts5InsertCtx ctx;              /* Tokenization callback context object */
  Fts5Buffer buf;                 /* Buffer used to build up %_docsize blob */

  memset(&buf, 0, sizeof(Fts5Buffer));
  rc = fts5StorageLoadTotals(p, 1);

  /* Insert the new row into the %_content table. */
  if( rc==SQLITE_OK ){
    if( pConfig->eContent!=FTS5_CONTENT_NORMAL ){
      if( sqlite3_value_type(apVal[1])==SQLITE_INTEGER ){
        *piRowid = sqlite3_value_int64(apVal[1]);
      }else{
        rc = fts5StorageNewRowid(p, piRowid);
      }
    }else{
      if( eConflict==SQLITE_REPLACE ){
        eStmt = FTS5_STMT_REPLACE_CONTENT;
        rc = fts5StorageDeleteFromIndex(p, sqlite3_value_int64(apVal[1]));
      }else{
        eStmt = FTS5_STMT_INSERT_CONTENT;
      }
      if( rc==SQLITE_OK ){
        rc = fts5StorageGetStmt(p, eStmt, &pInsert, 0);
      }
      for(i=1; rc==SQLITE_OK && i<=pConfig->nCol+1; i++){
        rc = sqlite3_bind_value(pInsert, i, apVal[i]);
      }
      if( rc==SQLITE_OK ){
        sqlite3_step(pInsert);
        rc = sqlite3_reset(pInsert);
      }
      *piRowid = sqlite3_last_insert_rowid(pConfig->db);
    }
  }

  /* Add new entries to the FTS index */
  if( rc==SQLITE_OK ){
    rc = sqlite3Fts5IndexBeginWrite(p->pIndex, *piRowid);
    ctx.pStorage = p;
  }
  for(ctx.iCol=0; rc==SQLITE_OK && ctx.iCol<pConfig->nCol; ctx.iCol++){
    ctx.szCol = 0;
    if( pConfig->abUnindexed[ctx.iCol]==0 ){
      rc = sqlite3Fts5Tokenize(pConfig, 
          (const char*)sqlite3_value_text(apVal[ctx.iCol+2]),
          sqlite3_value_bytes(apVal[ctx.iCol+2]),
          (void*)&ctx,
          fts5StorageInsertCallback
      );
    }
    sqlite3Fts5BufferAppendVarint(&rc, &buf, ctx.szCol);
    p->aTotalSize[ctx.iCol] += (i64)ctx.szCol;
  }
  p->nTotalRow++;

  /* Write the %_docsize record */
  if( rc==SQLITE_OK ){
    rc = fts5StorageInsertDocsize(p, *piRowid, &buf);
  }
  sqlite3_free(buf.p);

  /* Write the averages record */
  if( rc==SQLITE_OK ){
    rc = fts5StorageSaveTotals(p);
  }

  return rc;
}

static int fts5StorageCount(Fts5Storage *p, const char *zSuffix, i64 *pnRow){
  Fts5Config *pConfig = p->pConfig;
  char *zSql;
  int rc;

  zSql = sqlite3_mprintf("SELECT count(*) FROM %Q.'%q_%s'", 
      pConfig->zDb, pConfig->zName, zSuffix
  );
  if( zSql==0 ){
    rc = SQLITE_NOMEM;
  }else{
    sqlite3_stmt *pCnt = 0;
    rc = sqlite3_prepare_v2(pConfig->db, zSql, -1, &pCnt, 0);
    if( rc==SQLITE_OK ){
      if( SQLITE_ROW==sqlite3_step(pCnt) ){
        *pnRow = sqlite3_column_int64(pCnt, 0);
      }
      rc = sqlite3_finalize(pCnt);
    }
  }

  sqlite3_free(zSql);
  return rc;
}

/*
** Context object used by sqlite3Fts5StorageIntegrity().
*/
typedef struct Fts5IntegrityCtx Fts5IntegrityCtx;
struct Fts5IntegrityCtx {
  i64 iRowid;
  int iCol;
  int szCol;
  u64 cksum;
  Fts5Config *pConfig;
};

/*
** Tokenization callback used by integrity check.
*/
static int fts5StorageIntegrityCallback(
  void *pContext,                 /* Pointer to Fts5InsertCtx object */
  const char *pToken,             /* Buffer containing token */
  int nToken,                     /* Size of token in bytes */
  int iStart,                     /* Start offset of token */
  int iEnd                        /* End offset of token */
){
  Fts5IntegrityCtx *pCtx = (Fts5IntegrityCtx*)pContext;
  int iPos = pCtx->szCol++;
  pCtx->cksum ^= sqlite3Fts5IndexCksum(
      pCtx->pConfig, pCtx->iRowid, pCtx->iCol, iPos, pToken, nToken
  );
  return SQLITE_OK;
}

/*
** Check that the contents of the FTS index match that of the %_content
** table. Return SQLITE_OK if they do, or SQLITE_CORRUPT if not. Return
** some other SQLite error code if an error occurs while attempting to
** determine this.
*/
int sqlite3Fts5StorageIntegrity(Fts5Storage *p){
  Fts5Config *pConfig = p->pConfig;
  int rc;                         /* Return code */
  int *aColSize;                  /* Array of size pConfig->nCol */
  i64 *aTotalSize;                /* Array of size pConfig->nCol */
  Fts5IntegrityCtx ctx;
  sqlite3_stmt *pScan;

  memset(&ctx, 0, sizeof(Fts5IntegrityCtx));
  ctx.pConfig = p->pConfig;
  aTotalSize = (i64*)sqlite3_malloc(pConfig->nCol * (sizeof(int)+sizeof(i64)));
  if( !aTotalSize ) return SQLITE_NOMEM;
  aColSize = (int*)&aTotalSize[pConfig->nCol];
  memset(aTotalSize, 0, sizeof(i64) * pConfig->nCol);

  /* Generate the expected index checksum based on the contents of the
  ** %_content table. This block stores the checksum in ctx.cksum. */
  rc = fts5StorageGetStmt(p, FTS5_STMT_SCAN_ASC, &pScan, 0);
  if( rc==SQLITE_OK ){
    int rc2;
    while( SQLITE_ROW==sqlite3_step(pScan) ){
      int i;
      ctx.iRowid = sqlite3_column_int64(pScan, 0);
      ctx.szCol = 0;
      rc = sqlite3Fts5StorageDocsize(p, ctx.iRowid, aColSize);
      for(i=0; rc==SQLITE_OK && i<pConfig->nCol; i++){
        if( pConfig->abUnindexed[i] ) continue;
        ctx.iCol = i;
        ctx.szCol = 0;
        rc = sqlite3Fts5Tokenize(
            pConfig, 
            (const char*)sqlite3_column_text(pScan, i+1),
            sqlite3_column_bytes(pScan, i+1),
            (void*)&ctx,
            fts5StorageIntegrityCallback
        );
        if( ctx.szCol!=aColSize[i] ) rc = FTS5_CORRUPT;
        aTotalSize[i] += ctx.szCol;
      }
      if( rc!=SQLITE_OK ) break;
    }
    rc2 = sqlite3_reset(pScan);
    if( rc==SQLITE_OK ) rc = rc2;
  }

  /* Test that the "totals" (sometimes called "averages") record looks Ok */
  if( rc==SQLITE_OK ){
    int i;
    rc = fts5StorageLoadTotals(p, 0);
    for(i=0; rc==SQLITE_OK && i<pConfig->nCol; i++){
      if( p->aTotalSize[i]!=aTotalSize[i] ) rc = FTS5_CORRUPT;
    }
  }

  /* Check that the %_docsize and %_content tables contain the expected
  ** number of rows.  */
  if( rc==SQLITE_OK && pConfig->eContent==FTS5_CONTENT_NORMAL ){
    i64 nRow;
    rc = fts5StorageCount(p, "content", &nRow);
    if( rc==SQLITE_OK && nRow!=p->nTotalRow ) rc = FTS5_CORRUPT;
  }
  if( rc==SQLITE_OK ){
    i64 nRow;
    rc = fts5StorageCount(p, "docsize", &nRow);
    if( rc==SQLITE_OK && nRow!=p->nTotalRow ) rc = FTS5_CORRUPT;
  }

  /* Pass the expected checksum down to the FTS index module. It will
  ** verify, amongst other things, that it matches the checksum generated by
  ** inspecting the index itself.  */
  if( rc==SQLITE_OK ){
    rc = sqlite3Fts5IndexIntegrityCheck(p->pIndex, ctx.cksum);
  }

  sqlite3_free(aTotalSize);
  return rc;
}

/*
** Obtain an SQLite statement handle that may be used to read data from the
** %_content table.
*/
int sqlite3Fts5StorageStmt(
  Fts5Storage *p, 
  int eStmt, 
  sqlite3_stmt **pp, 
  char **pzErrMsg
){
  int rc;
  assert( eStmt==FTS5_STMT_SCAN_ASC 
       || eStmt==FTS5_STMT_SCAN_DESC
       || eStmt==FTS5_STMT_LOOKUP
  );
  rc = fts5StorageGetStmt(p, eStmt, pp, pzErrMsg);
  if( rc==SQLITE_OK ){
    assert( p->aStmt[eStmt]==*pp );
    p->aStmt[eStmt] = 0;
  }
  return rc;
}

/*
** Release an SQLite statement handle obtained via an earlier call to
** sqlite3Fts5StorageStmt(). The eStmt parameter passed to this function
** must match that passed to the sqlite3Fts5StorageStmt() call.
*/
void sqlite3Fts5StorageStmtRelease(
  Fts5Storage *p, 
  int eStmt, 
  sqlite3_stmt *pStmt
){
  assert( eStmt==FTS5_STMT_SCAN_ASC
       || eStmt==FTS5_STMT_SCAN_DESC
       || eStmt==FTS5_STMT_LOOKUP
  );
  if( p->aStmt[eStmt]==0 ){
    sqlite3_reset(pStmt);
    p->aStmt[eStmt] = pStmt;
  }else{
    sqlite3_finalize(pStmt);
  }
}

static int fts5StorageDecodeSizeArray(
  int *aCol, int nCol,            /* Array to populate */
  const u8 *aBlob, int nBlob      /* Record to read varints from */
){
  int i;
  int iOff = 0;
  for(i=0; i<nCol; i++){
    if( iOff>=nBlob ) return 1;
    iOff += getVarint32(&aBlob[iOff], aCol[i]);
  }
  return (iOff!=nBlob);
}

/*
** Argument aCol points to an array of integers containing one entry for
** each table column. This function reads the %_docsize record for the
** specified rowid and populates aCol[] with the results.
**
** An SQLite error code is returned if an error occurs, or SQLITE_OK
** otherwise.
*/
int sqlite3Fts5StorageDocsize(Fts5Storage *p, i64 iRowid, int *aCol){
  int nCol = p->pConfig->nCol;
  sqlite3_stmt *pLookup = 0;
  int rc = fts5StorageGetStmt(p, FTS5_STMT_LOOKUP_DOCSIZE, &pLookup, 0);
  if( rc==SQLITE_OK ){
    int bCorrupt = 1;
    sqlite3_bind_int64(pLookup, 1, iRowid);
    if( SQLITE_ROW==sqlite3_step(pLookup) ){
      const u8 *aBlob = sqlite3_column_blob(pLookup, 0);
      int nBlob = sqlite3_column_bytes(pLookup, 0);
      if( 0==fts5StorageDecodeSizeArray(aCol, nCol, aBlob, nBlob) ){
        bCorrupt = 0;
      }
    }
    rc = sqlite3_reset(pLookup);
    if( bCorrupt && rc==SQLITE_OK ){
      rc = FTS5_CORRUPT;
    }
  }
  return rc;
}

int sqlite3Fts5StorageSize(Fts5Storage *p, int iCol, i64 *pnToken){
  int rc = fts5StorageLoadTotals(p, 0);
  if( rc==SQLITE_OK ){
    *pnToken = 0;
    if( iCol<0 ){
      int i;
      for(i=0; i<p->pConfig->nCol; i++){
        *pnToken += p->aTotalSize[i];
      }
    }else if( iCol<p->pConfig->nCol ){
      *pnToken = p->aTotalSize[iCol];
    }else{
      rc = SQLITE_RANGE;
    }
  }
  return rc;
}

int sqlite3Fts5StorageRowCount(Fts5Storage *p, i64 *pnRow){
  int rc = fts5StorageLoadTotals(p, 0);
  if( rc==SQLITE_OK ){
    *pnRow = p->nTotalRow;
  }
  return rc;
}

/*
** Flush any data currently held in-memory to disk.
*/
int sqlite3Fts5StorageSync(Fts5Storage *p, int bCommit){
  if( bCommit && p->bTotalsValid ){
    int rc = fts5StorageSaveTotals(p);
    p->bTotalsValid = 0;
    if( rc!=SQLITE_OK ) return rc;
  }
  return sqlite3Fts5IndexSync(p->pIndex, bCommit);
}

int sqlite3Fts5StorageRollback(Fts5Storage *p){
  p->bTotalsValid = 0;
  return sqlite3Fts5IndexRollback(p->pIndex);
}

int sqlite3Fts5StorageConfigValue(
  Fts5Storage *p, 
  const char *z,
  sqlite3_value *pVal,
  int iVal
){
  sqlite3_stmt *pReplace = 0;
  int rc = fts5StorageGetStmt(p, FTS5_STMT_REPLACE_CONFIG, &pReplace, 0);
  if( rc==SQLITE_OK ){
    sqlite3_bind_text(pReplace, 1, z, -1, SQLITE_STATIC);
    if( pVal ){
      sqlite3_bind_value(pReplace, 2, pVal);
    }else{
      sqlite3_bind_int(pReplace, 2, iVal);
    }
    sqlite3_step(pReplace);
    rc = sqlite3_reset(pReplace);
  }
  if( rc==SQLITE_OK && pVal ){
    int iNew = p->pConfig->iCookie + 1;
    rc = sqlite3Fts5IndexSetCookie(p->pIndex, iNew);
    if( rc==SQLITE_OK ){
      p->pConfig->iCookie = iNew;
    }
  }
  return rc;
}


#endif /* SQLITE_ENABLE_FTS5 */