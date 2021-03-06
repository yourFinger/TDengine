/*
 * Copyright (c) 2019 TAOS Data, Inc. <jhtao@taosdata.com>
 *
 * This program is free software: you can use, redistribute, and/or modify
 * it under the terms of the GNU Affero General Public License, version 3
 * or later ("AGPL"), as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include <tschemautil.h>
#include "os.h"
#include "taosmsg.h"
#include "tscLog.h"
#include "tscUtil.h"
#include "tsched.h"
#include "tcache.h"
#include "tsclient.h"
#include "ttimer.h"
#include "tutil.h"

#include "tscProfile.h"

static void tscProcessStreamQueryCallback(void *param, TAOS_RES *tres, int numOfRows);
static void tscProcessStreamRetrieveResult(void *param, TAOS_RES *res, int numOfRows);
static void tscSetNextLaunchTimer(SSqlStream *pStream, SSqlObj *pSql);
static void tscSetRetryTimer(SSqlStream *pStream, SSqlObj *pSql, int64_t timer);

static int64_t getDelayValueAfterTimewindowClosed(SSqlStream* pStream, int64_t launchDelay) {
  return taosGetTimestamp(pStream->precision) + launchDelay - pStream->stime - 1;
}

static bool isProjectStream(SQueryInfo* pQueryInfo) {
  for (int32_t i = 0; i < pQueryInfo->fieldsInfo.numOfOutput; ++i) {
    SSqlExpr *pExpr = tscSqlExprGet(pQueryInfo, i);
    if (pExpr->functionId != TSDB_FUNC_PRJ) {
      return false;
    }
  }

  return true;
}

static int64_t tscGetRetryDelayTime(int64_t slidingTime, int16_t prec) {
  float retryRangeFactor = 0.3;

  // change to ms
  if (prec == TSDB_TIME_PRECISION_MICRO) {
    slidingTime = slidingTime / 1000;
  }

  int64_t retryDelta = (int64_t)tsStreamCompRetryDelay * retryRangeFactor;
  retryDelta = ((rand() % retryDelta) + tsStreamCompRetryDelay) * 1000L;

  if (slidingTime < retryDelta) {
    return slidingTime;
  } else {
    return retryDelta;
  }
}

static void tscProcessStreamLaunchQuery(SSchedMsg *pMsg) {
  SSqlStream *pStream = (SSqlStream *)pMsg->ahandle;
  SSqlObj *   pSql = pStream->pSql;

  pSql->fp = tscProcessStreamQueryCallback;
  pSql->fetchFp = tscProcessStreamQueryCallback;
  pSql->param = pStream;
  pSql->res.completed = false;
  
  SQueryInfo *pQueryInfo = tscGetQueryInfoDetail(&pSql->cmd, 0);
  STableMetaInfo *pTableMetaInfo = tscGetMetaInfo(pQueryInfo, 0);

  int code = tscGetTableMeta(pSql, pTableMetaInfo);
  pSql->res.code = code;

  if (code == 0 && UTIL_TABLE_IS_SUPER_TABLE(pTableMetaInfo)) {
    code = tscGetSTableVgroupInfo(pSql, 0);
    pSql->res.code = code;
  }

  // failed to get meter/metric meta, retry in 10sec.
  if (code != TSDB_CODE_SUCCESS) {
    int64_t retryDelayTime = tscGetRetryDelayTime(pStream->slidingTime, pStream->precision);
    tscDebug("%p stream:%p,get metermeta failed, retry in %" PRId64 "ms", pStream->pSql, pStream, retryDelayTime);
    tscSetRetryTimer(pStream, pSql, retryDelayTime);

  } else {
    tscTansformSQLFuncForSTableQuery(pQueryInfo);
    tscDebug("%p stream:%p start stream query on:%s", pSql, pStream, pTableMetaInfo->name);
    tscDoQuery(pStream->pSql);
    tscIncStreamExecutionCount(pStream);
  }
}

static void tscProcessStreamTimer(void *handle, void *tmrId) {
  SSqlStream *pStream = (SSqlStream *)handle;
  if (pStream == NULL) return;
  if (pStream->pTimer != tmrId) return;
  pStream->pTimer = NULL;

  pStream->numOfRes = 0;  // reset the numOfRes.
  SSqlObj *pSql = pStream->pSql;
  SQueryInfo* pQueryInfo = tscGetQueryInfoDetail(&pSql->cmd, 0);
  tscDebug("%p add into timer", pSql);

  if (pStream->isProject) {
    /*
     * pQueryInfo->window.ekey, which is the start time, does not change in case of
     * repeat first execution, once the first execution failed.
     */
    pQueryInfo->window.skey = pStream->stime;  // start time

    pQueryInfo->window.ekey = taosGetTimestamp(pStream->precision);  // end time
    if (pQueryInfo->window.ekey > pStream->etime) {
      pQueryInfo->window.ekey = pStream->etime;
    }
  } else {
    pQueryInfo->window.skey = pStream->stime;
    int64_t etime = taosGetTimestamp(pStream->precision);
    // delay to wait all data in last time window
    if (pStream->precision == TSDB_TIME_PRECISION_MICRO) {
      etime -= tsMaxStreamComputDelay * 1000l;
    } else {
      etime -= tsMaxStreamComputDelay;
    }
    if (etime > pStream->etime) {
      etime = pStream->etime;
    } else {
      etime = pStream->stime + (etime - pStream->stime) / pStream->interval * pStream->interval;
    }
    pQueryInfo->window.ekey = etime;
  }

  // launch stream computing in a new thread
  SSchedMsg schedMsg = { 0 };
  schedMsg.fp = tscProcessStreamLaunchQuery;
  schedMsg.ahandle = pStream;
  schedMsg.thandle = (void *)1;
  schedMsg.msg = NULL;
  taosScheduleTask(tscQhandle, &schedMsg);
}

static void tscProcessStreamQueryCallback(void *param, TAOS_RES *tres, int numOfRows) {
  SSqlStream *pStream = (SSqlStream *)param;
  if (tres == NULL || numOfRows < 0) {
    int64_t retryDelay = tscGetRetryDelayTime(pStream->slidingTime, pStream->precision);
    tscError("%p stream:%p, query data failed, code:0x%08x, retry in %" PRId64 "ms", pStream->pSql, pStream, numOfRows,
             retryDelay);

    STableMetaInfo* pTableMetaInfo = tscGetTableMetaInfoFromCmd(&pStream->pSql->cmd, 0, 0);
    taosCacheRelease(tscCacheHandle, (void**)&(pTableMetaInfo->pTableMeta), true);
    taosTFree(pTableMetaInfo->vgroupList);
  
    tscSetRetryTimer(pStream, pStream->pSql, retryDelay);
    return;
  }

  taos_fetch_rows_a(tres, tscProcessStreamRetrieveResult, param);
}

// no need to be called as this is alreay done in the query
static void tscStreamFillTimeGap(SSqlStream* pStream, TSKEY ts) {
#if 0
  SSqlObj *   pSql = pStream->pSql;
  SQueryInfo* pQueryInfo = tscGetQueryInfoDetail(&pSql->cmd, 0);
  
  if (pQueryInfo->fillType != TSDB_FILL_SET_VALUE && pQueryInfo->fillType != TSDB_FILL_NULL) {
    return;
  }

  SSqlRes *pRes = &pSql->res;
  /* failed to retrieve any result in this retrieve */
  pSql->res.numOfRows = 1;
  void *row[TSDB_MAX_COLUMNS] = {0};
  char  tmpRes[TSDB_MAX_BYTES_PER_ROW] = {0};
  void *oldPtr = pSql->res.data;
  pSql->res.data = tmpRes;
  int32_t rowNum = 0;

  while (pStream->stime + pStream->slidingTime < ts) {
    pStream->stime += pStream->slidingTime;
    *(TSKEY*)row[0] =  pStream->stime;
    for (int32_t i = 1; i < pQueryInfo->fieldsInfo.numOfOutput; ++i) {
      int16_t     offset = tscFieldInfoGetOffset(pQueryInfo, i);
      TAOS_FIELD *pField = tscFieldInfoGetField(&pQueryInfo->fieldsInfo, i);
      assignVal(pSql->res.data + offset, (char *)(&pQueryInfo->fillVal[i]), pField->bytes, pField->type);
      row[i] = pSql->res.data + offset;
    }
    (*pStream->fp)(pStream->param, pSql, row);
    ++rowNum;
  }

  if (rowNum > 0) {
    tscDebug("%p stream:%p %d rows padded", pSql, pStream, rowNum);
  }

  pRes->numOfRows = 0;
  pRes->data = oldPtr;
#endif
}

static void tscProcessStreamRetrieveResult(void *param, TAOS_RES *res, int numOfRows) {
  SSqlStream *    pStream = (SSqlStream *)param;
  SSqlObj *       pSql = (SSqlObj *)res;

  if (pSql == NULL || numOfRows < 0) {
    int64_t retryDelayTime = tscGetRetryDelayTime(pStream->slidingTime, pStream->precision);
    tscError("%p stream:%p, retrieve data failed, code:0x%08x, retry in %" PRId64 "ms", pSql, pStream, numOfRows, retryDelayTime);
  
    tscSetRetryTimer(pStream, pStream->pSql, retryDelayTime);
    return;
  }

  STableMetaInfo *pTableMetaInfo = tscGetTableMetaInfoFromCmd(&pSql->cmd, 0, 0);

  if (numOfRows > 0) { // when reaching here the first execution of stream computing is successful.
    pStream->numOfRes += numOfRows;
    for(int32_t i = 0; i < numOfRows; ++i) {
      TAOS_ROW row = taos_fetch_row(res);
      tscDebug("%p stream:%p fetch result", pSql, pStream);
      tscStreamFillTimeGap(pStream, *(TSKEY*)row[0]);
      pStream->stime = *(TSKEY *)row[0];

      // user callback function
      (*pStream->fp)(pStream->param, res, row);
    }

    if (!pStream->isProject) {
      pStream->stime += pStream->slidingTime;
    }
    // actually only one row is returned. this following is not necessary
    taos_fetch_rows_a(res, tscProcessStreamRetrieveResult, pStream);
  } else {  // numOfRows == 0, all data has been retrieved
    pStream->useconds += pSql->res.useconds;
    if (pStream->numOfRes == 0) {
      if (pStream->isProject) {
        /* no resuls in the query range, retry */
        // todo set retry dynamic time
        int32_t retry = tsProjectExecInterval;
        tscError("%p stream:%p, retrieve no data, code:0x%08x, retry in %" PRId32 "ms", pSql, pStream, numOfRows, retry);

        tscSetRetryTimer(pStream, pStream->pSql, retry);
        return;
      }
    } else if (pStream->isProject) {
      pStream->stime += 1;
    }

    tscDebug("%p stream:%p, query on:%s, fetch result completed, fetched rows:%" PRId64, pSql, pStream, pTableMetaInfo->name,
             pStream->numOfRes);

    // release the metric/meter meta information reference, so data in cache can be updated

    taosCacheRelease(tscCacheHandle, (void**)&(pTableMetaInfo->pTableMeta), false);
    tscFreeSqlResult(pSql);
    taosTFree(pSql->pSubs);
    pSql->numOfSubs = 0;
    taosTFree(pTableMetaInfo->vgroupList);
    tscSetNextLaunchTimer(pStream, pSql);
  }
}

static void tscSetRetryTimer(SSqlStream *pStream, SSqlObj *pSql, int64_t timer) {
  int64_t delay = getDelayValueAfterTimewindowClosed(pStream, timer);
  
  if (pStream->isProject) {
    int64_t now = taosGetTimestamp(pStream->precision);
    int64_t etime = now > pStream->etime ? pStream->etime : now;

    if (pStream->etime < now && now - pStream->etime > tsMaxRetentWindow) {
      /*
       * current time window will be closed, since it too early to exceed the maxRetentWindow value
       */
      tscDebug("%p stream:%p, etime:%" PRId64 " is too old, exceeds the max retention time window:%" PRId64 ", stop the stream",
               pStream->pSql, pStream, pStream->stime, pStream->etime);
      // TODO : How to terminate stream here
      if (pStream->callback) {
        // Callback function from upper level
        pStream->callback(pStream->param);
      }
      taos_close_stream(pStream);
      return;
    }
  
    tscDebug("%p stream:%p, next start at %" PRId64 ", in %" PRId64 "ms. delay:%" PRId64 "ms qrange %" PRId64 "-%" PRId64, pStream->pSql, pStream,
             now + timer, timer, delay, pStream->stime, etime);
  } else {
    tscDebug("%p stream:%p, next start at %" PRId64 ", in %" PRId64 "ms. delay:%" PRId64 "ms qrange %" PRId64 "-%" PRId64, pStream->pSql, pStream,
             pStream->stime, timer, delay, pStream->stime - pStream->interval, pStream->stime - 1);
  }

  pSql->cmd.command = TSDB_SQL_SELECT;

  // start timer for next computing
  taosTmrReset(tscProcessStreamTimer, timer, pStream, tscTmr, &pStream->pTimer);
}

static int64_t getLaunchTimeDelay(const SSqlStream* pStream) {
  int64_t delayDelta = (int64_t)(pStream->slidingTime * tsStreamComputDelayRatio);
  
  int64_t maxDelay =
      (pStream->precision == TSDB_TIME_PRECISION_MICRO) ? tsMaxStreamComputDelay * 1000L : tsMaxStreamComputDelay;
  
  if (delayDelta > maxDelay) {
    delayDelta = maxDelay;
  }
  
  int64_t remainTimeWindow = pStream->slidingTime - delayDelta;
  if (maxDelay > remainTimeWindow) {
    maxDelay = (remainTimeWindow / 1.5);
  }
  
  int64_t currentDelay = (rand() % maxDelay);  // a random number
  currentDelay += delayDelta;
  assert(currentDelay < pStream->slidingTime);
  
  return currentDelay;
}


static void tscSetNextLaunchTimer(SSqlStream *pStream, SSqlObj *pSql) {
  int64_t timer = 0;
  
  if (pStream->isProject) {
    /*
     * for project query, no mater fetch data successfully or not, next launch will issue
     * more than the sliding time window
     */
    timer = pStream->slidingTime;
    if (pStream->stime > pStream->etime) {
      tscDebug("%p stream:%p, stime:%" PRId64 " is larger than end time: %" PRId64 ", stop the stream", pStream->pSql, pStream,
               pStream->stime, pStream->etime);
      // TODO : How to terminate stream here
      if (pStream->callback) {
        // Callback function from upper level
        pStream->callback(pStream->param);
      }
      taos_close_stream(pStream);
      return;
    }
  } else {
    if ((pStream->stime - pStream->interval) >= pStream->etime) {
      tscDebug("%p stream:%p, stime:%" PRId64 " is larger than end time: %" PRId64 ", stop the stream", pStream->pSql, pStream,
               pStream->stime, pStream->etime);
      // TODO : How to terminate stream here
      if (pStream->callback) {
        // Callback function from upper level
        pStream->callback(pStream->param);
      }
      taos_close_stream(pStream);
      return;
    }
    
    timer = pStream->stime - taosGetTimestamp(pStream->precision);
    if (timer < 0) {
      timer = 0;
    }
  }

  timer += getLaunchTimeDelay(pStream);
  
  if (pStream->precision == TSDB_TIME_PRECISION_MICRO) {
    timer = timer / 1000L;
  }

  tscSetRetryTimer(pStream, pSql, timer);
}

static void tscSetSlidingWindowInfo(SSqlObj *pSql, SSqlStream *pStream) {
  int64_t minIntervalTime =
      (pStream->precision == TSDB_TIME_PRECISION_MICRO) ? tsMinIntervalTime * 1000L : tsMinIntervalTime;
  
  SQueryInfo* pQueryInfo = tscGetQueryInfoDetail(&pSql->cmd, 0);
  
  if (pQueryInfo->intervalTime < minIntervalTime) {
    tscWarn("%p stream:%p, original sample interval:%ld too small, reset to:%" PRId64, pSql, pStream,
            pQueryInfo->intervalTime, minIntervalTime);
    pQueryInfo->intervalTime = minIntervalTime;
  }

  pStream->interval = pQueryInfo->intervalTime;  // it shall be derived from sql string

  if (pQueryInfo->slidingTime == 0) {
    pQueryInfo->slidingTime = pQueryInfo->intervalTime;
  }

  int64_t minSlidingTime =
      (pStream->precision == TSDB_TIME_PRECISION_MICRO) ? tsMinSlidingTime * 1000L : tsMinSlidingTime;

  if (pQueryInfo->slidingTime == -1) {
    pQueryInfo->slidingTime = pQueryInfo->intervalTime;
  } else if (pQueryInfo->slidingTime < minSlidingTime) {
    tscWarn("%p stream:%p, original sliding value:%" PRId64 " too small, reset to:%" PRId64, pSql, pStream,
        pQueryInfo->slidingTime, minSlidingTime);

    pQueryInfo->slidingTime = minSlidingTime;
  }

  if (pQueryInfo->slidingTime > pQueryInfo->intervalTime) {
    tscWarn("%p stream:%p, sliding value:%" PRId64 " can not be larger than interval range, reset to:%" PRId64, pSql, pStream,
            pQueryInfo->slidingTime, pQueryInfo->intervalTime);

    pQueryInfo->slidingTime = pQueryInfo->intervalTime;
  }

  pStream->slidingTime = pQueryInfo->slidingTime;
  
  if (pStream->isProject) {
    pQueryInfo->intervalTime = 0; // clear the interval value to avoid the force time window split by query processor
    pQueryInfo->slidingTime = 0;
  }
}

static int64_t tscGetStreamStartTimestamp(SSqlObj *pSql, SSqlStream *pStream, int64_t stime) {
  SQueryInfo* pQueryInfo = tscGetQueryInfoDetail(&pSql->cmd, 0);
  
  if (pStream->isProject) {
    // no data in table, flush all data till now to destination meter, 10sec delay
    pStream->interval = tsProjectExecInterval;
    pStream->slidingTime = tsProjectExecInterval;

    if (stime != 0) {  // first projection start from the latest event timestamp
      assert(stime >= pQueryInfo->window.skey);
      stime += 1;  // exclude the last records from table
    } else {
      stime = pQueryInfo->window.skey;
    }
  } else {             // timewindow based aggregation stream
    if (stime == 0) {  // no data in meter till now
      stime = ((int64_t)taosGetTimestamp(pStream->precision) / pStream->interval) * pStream->interval;
      stime -= pStream->interval;
      tscWarn("%p stream:%p, last timestamp:0, reset to:%" PRId64, pSql, pStream, stime);
    } else {
      int64_t newStime = (stime / pStream->interval) * pStream->interval;
      if (newStime != stime) {
        tscWarn("%p stream:%p, last timestamp:%" PRId64 ", reset to:%" PRId64, pSql, pStream, stime, newStime);
        stime = newStime;
      }
    }
  }

  return stime;
}

static int64_t tscGetLaunchTimestamp(const SSqlStream *pStream) {
  int64_t timer = pStream->stime - taosGetTimestamp(pStream->precision);
  if (timer < 0) timer = 0;

  int64_t startDelay =
      (pStream->precision == TSDB_TIME_PRECISION_MICRO) ? tsStreamCompStartDelay * 1000L : tsStreamCompStartDelay;
  
  timer += getLaunchTimeDelay(pStream);
  timer += startDelay;
  
  return (pStream->precision == TSDB_TIME_PRECISION_MICRO) ? timer / 1000L : timer;
}

static void setErrorInfo(SSqlObj* pSql, int32_t code, char* info) {
  if (pSql == NULL) {
    return;
  }

  SSqlCmd* pCmd = &pSql->cmd;

  pSql->res.code = code;
  
  if (info != NULL) {
    strncpy(pCmd->payload, info, pCmd->payloadLen);
  }
}

static void tscCreateStream(void *param, TAOS_RES *res, int code) {
  SSqlStream* pStream = (SSqlStream*)param;
  SSqlObj* pSql = pStream->pSql;
  SSqlCmd* pCmd = &pSql->cmd;

  if (code != TSDB_CODE_SUCCESS) {
    setErrorInfo(pSql, code, pCmd->payload);
    tscError("%p open stream failed, sql:%s, reason:%s, code:0x%08x", pSql, pSql->sqlstr, pCmd->payload, code);
    pStream->fp(pStream->param, NULL, NULL);
    return;
  }

  SQueryInfo* pQueryInfo = tscGetQueryInfoDetail(pCmd, 0);
  STableMetaInfo* pTableMetaInfo = tscGetMetaInfo(pQueryInfo, 0);
  STableComInfo tinfo = tscGetTableInfo(pTableMetaInfo->pTableMeta);
  
  pStream->isProject = isProjectStream(pQueryInfo);
  pStream->precision = tinfo.precision;

  pStream->ctime = taosGetTimestamp(pStream->precision);
  pStream->etime = pQueryInfo->window.ekey;

  tscAddIntoStreamList(pStream);

  tscSetSlidingWindowInfo(pSql, pStream);
  pStream->stime = tscGetStreamStartTimestamp(pSql, pStream, pStream->stime);

  int64_t starttime = tscGetLaunchTimestamp(pStream);
  pCmd->command = TSDB_SQL_SELECT;
  taosTmrReset(tscProcessStreamTimer, starttime, pStream, tscTmr, &pStream->pTimer);

  tscDebug("%p stream:%p is opened, query on:%s, interval:%" PRId64 ", sliding:%" PRId64 ", first launched in:%" PRId64 ", sql:%s", pSql,
           pStream, pTableMetaInfo->name, pStream->interval, pStream->slidingTime, starttime, pSql->sqlstr);
}

TAOS_STREAM *taos_open_stream(TAOS *taos, const char *sqlstr, void (*fp)(void *param, TAOS_RES *, TAOS_ROW row),
                              int64_t stime, void *param, void (*callback)(void *)) {
  STscObj *pObj = (STscObj *)taos;
  if (pObj == NULL || pObj->signature != pObj) return NULL;

  SSqlObj *pSql = (SSqlObj *)calloc(1, sizeof(SSqlObj));
  if (pSql == NULL) {
    return NULL;
  }

  pSql->signature = pSql;
  pSql->pTscObj = pObj;

  SSqlCmd *pCmd = &pSql->cmd;
  SSqlRes *pRes = &pSql->res;

  SSqlStream *pStream = (SSqlStream *)calloc(1, sizeof(SSqlStream));
  if (pStream == NULL) {
    tscError("%p open stream failed, sql:%s, reason:%s, code:0x%08x", pSql, sqlstr, pCmd->payload, pRes->code);
    tscFreeSqlObj(pSql);
    return NULL;
  }

  pStream->stime = stime;
  pStream->fp = fp;
  pStream->callback = callback;
  pStream->param = param;
  pStream->pSql = pSql;
  pSql->pStream = pStream;
  pSql->param = pStream;

  pSql->sqlstr = calloc(1, strlen(sqlstr) + 1);
  if (pSql->sqlstr == NULL) {
    tscError("%p failed to malloc sql string buffer", pSql);
    tscFreeSqlObj(pSql);
    return NULL;
  }
  strtolower(pSql->sqlstr, sqlstr);

  tscDebugL("%p SQL: %s", pSql, pSql->sqlstr);
  tsem_init(&pSql->rspSem, 0, 0);

  pSql->fp = tscCreateStream;
  pSql->fetchFp = tscCreateStream;
  int32_t code = tsParseSql(pSql, true);
  if (code == TSDB_CODE_SUCCESS) {
    tscCreateStream(pStream, pSql, code);
  } else if (code != TSDB_CODE_TSC_ACTION_IN_PROGRESS) {
    tscError("%p open stream failed, sql:%s, code:%s", pSql, sqlstr, tstrerror(pRes->code));
    tscFreeSqlObj(pSql);
    free(pStream);
    return NULL;
  }

  return pStream;
}

void taos_close_stream(TAOS_STREAM *handle) {
  SSqlStream *pStream = (SSqlStream *)handle;

  SSqlObj *pSql = (SSqlObj *)atomic_exchange_ptr(&pStream->pSql, 0);
  if (pSql == NULL) {
    return;
  }

  /*
   * stream may be closed twice, 1. drop dst table, 2. kill stream
   * Here, we need a check before release memory
   */
  if (pSql->signature == pSql) {
    tscRemoveFromStreamList(pStream, pSql);

    taosTmrStopA(&(pStream->pTimer));

    tscDebug("%p stream:%p is closed", pSql, pStream);
    // notify CQ to release the pStream object
    pStream->fp(pStream->param, NULL, NULL);

    tscFreeSqlObj(pSql);
    pStream->pSql = NULL;

    taosTFree(pStream);
  }
}
