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

#include "dataSinkMgt.h"
#include "os.h"
#include "tmsg.h"
#include "tref.h"
#include "tudf.h"

#include "executor.h"
#include "executorimpl.h"
#include "query.h"

static TdThreadOnce initPoolOnce = PTHREAD_ONCE_INIT;
int32_t             exchangeObjRefPool = -1;

static void initRefPool() { exchangeObjRefPool = taosOpenRef(1024, doDestroyExchangeOperatorInfo); }
static void cleanupRefPool() {
  int32_t ref = atomic_val_compare_exchange_32(&exchangeObjRefPool, exchangeObjRefPool, 0);
  taosCloseRef(ref);
}

int32_t qCreateExecTask(SReadHandle* readHandle, int32_t vgId, uint64_t taskId, SSubplan* pSubplan,
                        qTaskInfo_t* pTaskInfo, DataSinkHandle* handle, const char* sql, EOPTR_EXEC_MODEL model) {
  assert(pSubplan != NULL);
  SExecTaskInfo** pTask = (SExecTaskInfo**)pTaskInfo;

  taosThreadOnce(&initPoolOnce, initRefPool);
  atexit(cleanupRefPool);
  int32_t code = createExecTaskInfoImpl(pSubplan, pTask, readHandle, taskId, sql, model);
  if (code != TSDB_CODE_SUCCESS) {
    goto _error;
  }

  if (model == OPTR_EXEC_MODEL_STREAM) {
    (*pTask)->streamInfo.inputQueue = streamQueueOpen();
    if ((*pTask)->streamInfo.inputQueue == NULL) {
      goto _error;
    }
  }

  SDataSinkMgtCfg cfg = {.maxDataBlockNum = 1000, .maxDataBlockNumPerQuery = 100};
  code = dsDataSinkMgtInit(&cfg);
  if (code != TSDB_CODE_SUCCESS) {
    goto _error;
  }

  if (handle) {
    void* pSinkParam = NULL;
    code = createDataSinkParam(pSubplan->pDataSink, &pSinkParam, pTaskInfo, readHandle);
    if (code != TSDB_CODE_SUCCESS) {
      goto _error;
    }

    code = dsCreateDataSinker(pSubplan->pDataSink, handle, pSinkParam);
  }

_error:
  // if failed to add ref for all tables in this query, abort current query
  return code;
}

#ifdef TEST_IMPL
// wait moment
int waitMoment(SQInfo* pQInfo) {
  if (pQInfo->sql) {
    int   ms = 0;
    char* pcnt = strstr(pQInfo->sql, " count(*)");
    if (pcnt) return 0;

    char* pos = strstr(pQInfo->sql, " t_");
    if (pos) {
      pos += 3;
      ms = atoi(pos);
      while (*pos >= '0' && *pos <= '9') {
        pos++;
      }
      char unit_char = *pos;
      if (unit_char == 'h') {
        ms *= 3600 * 1000;
      } else if (unit_char == 'm') {
        ms *= 60 * 1000;
      } else if (unit_char == 's') {
        ms *= 1000;
      }
    }
    if (ms == 0) return 0;
    printf("test wait sleep %dms. sql=%s ...\n", ms, pQInfo->sql);

    if (ms < 1000) {
      taosMsleep(ms);
    } else {
      int used_ms = 0;
      while (used_ms < ms) {
        taosMsleep(1000);
        used_ms += 1000;
        if (isTaskKilled(pQInfo)) {
          printf("test check query is canceled, sleep break.%s\n", pQInfo->sql);
          break;
        }
      }
    }
  }
  return 1;
}
#endif

int32_t qExecTask(qTaskInfo_t tinfo, SSDataBlock** pRes, uint64_t* useconds) {
  SExecTaskInfo* pTaskInfo = (SExecTaskInfo*)tinfo;
  int64_t        threadId = taosGetSelfPthreadId();

  *pRes = NULL;
  int64_t curOwner = 0;
  if ((curOwner = atomic_val_compare_exchange_64(&pTaskInfo->owner, 0, threadId)) != 0) {
    qError("%s-%p execTask is now executed by thread:%p", GET_TASKID(pTaskInfo), pTaskInfo, (void*)curOwner);
    pTaskInfo->code = TSDB_CODE_QRY_IN_EXEC;
    return pTaskInfo->code;
  }

  if (pTaskInfo->cost.start == 0) {
    pTaskInfo->cost.start = taosGetTimestampMs();
  }

  if (isTaskKilled(pTaskInfo)) {
    qDebug("%s already killed, abort", GET_TASKID(pTaskInfo));
    return TSDB_CODE_SUCCESS;
  }

  // error occurs, record the error code and return to client
  int32_t ret = setjmp(pTaskInfo->env);
  if (ret != TSDB_CODE_SUCCESS) {
    pTaskInfo->code = ret;
    cleanUpUdfs();
    qDebug("%s task abort due to error/cancel occurs, code:%s", GET_TASKID(pTaskInfo), tstrerror(pTaskInfo->code));
    return pTaskInfo->code;
  }

  qDebug("%s execTask is launched", GET_TASKID(pTaskInfo));

  int64_t st = taosGetTimestampUs();

  *pRes = pTaskInfo->pRoot->fpSet.getNextFn(pTaskInfo->pRoot);
  uint64_t el = (taosGetTimestampUs() - st);

  pTaskInfo->cost.elapsedTime += el;
  if (NULL == *pRes) {
    *useconds = pTaskInfo->cost.elapsedTime;
  }

  cleanUpUdfs();

  int32_t  current = (*pRes != NULL) ? (*pRes)->info.rows : 0;
  uint64_t total = pTaskInfo->pRoot->resultInfo.totalRows;

  qDebug("%s task suspended, %d rows returned, total:%" PRId64 " rows, in sinkNode:%d, elapsed:%.2f ms",
         GET_TASKID(pTaskInfo), current, total, 0, el / 1000.0);

  atomic_store_64(&pTaskInfo->owner, 0);
  return pTaskInfo->code;
}

int32_t qKillTask(qTaskInfo_t qinfo) {
  SExecTaskInfo* pTaskInfo = (SExecTaskInfo*)qinfo;

  if (pTaskInfo == NULL) {
    return TSDB_CODE_QRY_INVALID_QHANDLE;
  }

  qDebug("%s execTask killed", GET_TASKID(pTaskInfo));
  setTaskKilled(pTaskInfo);

  // Wait for the query executing thread being stopped/
  // Once the query is stopped, the owner of qHandle will be cleared immediately.
  while (pTaskInfo->owner != 0) {
    taosMsleep(100);
  }

  return TSDB_CODE_SUCCESS;
}

int32_t qAsyncKillTask(qTaskInfo_t qinfo) {
  SExecTaskInfo* pTaskInfo = (SExecTaskInfo*)qinfo;

  if (pTaskInfo == NULL) {
    return TSDB_CODE_QRY_INVALID_QHANDLE;
  }

  qDebug("%s execTask async killed", GET_TASKID(pTaskInfo));
  setTaskKilled(pTaskInfo);

  return TSDB_CODE_SUCCESS;
}

void qDestroyTask(qTaskInfo_t qTaskHandle) {
  SExecTaskInfo* pTaskInfo = (SExecTaskInfo*)qTaskHandle;
  qDebug("%s execTask completed, numOfRows:%" PRId64, GET_TASKID(pTaskInfo), pTaskInfo->pRoot->resultInfo.totalRows);

  queryCostStatis(pTaskInfo);  // print the query cost summary
  doDestroyTask(pTaskInfo);
}

int32_t qGetExplainExecInfo(qTaskInfo_t tinfo, int32_t* resNum, SExplainExecInfo** pRes) {
  SExecTaskInfo* pTaskInfo = (SExecTaskInfo*)tinfo;
  int32_t        capacity = 0;

  return getOperatorExplainExecInfo(pTaskInfo->pRoot, pRes, &capacity, resNum);
}

int32_t qSerializeTaskStatus(qTaskInfo_t tinfo, char** pOutput, int32_t* len) {
  SExecTaskInfo* pTaskInfo = (struct SExecTaskInfo*)tinfo;
  if (pTaskInfo->pRoot == NULL) {
    return TSDB_CODE_INVALID_PARA;
  }

  int32_t nOptrWithVal = 0;
  int32_t code = encodeOperator(pTaskInfo->pRoot, pOutput, len, &nOptrWithVal);
  if ((code == TSDB_CODE_SUCCESS) && (nOptrWithVal = 0)) {
    taosMemoryFreeClear(*pOutput);
    *len = 0;
  }
  return code;
}

int32_t qDeserializeTaskStatus(qTaskInfo_t tinfo, const char* pInput, int32_t len) {
  SExecTaskInfo* pTaskInfo = (struct SExecTaskInfo*)tinfo;

  if (pTaskInfo == NULL || pInput == NULL || len == 0) {
    return TSDB_CODE_INVALID_PARA;
  }

  return decodeOperator(pTaskInfo->pRoot, pInput, len);
}

int32_t qExtractStreamScanner(qTaskInfo_t tinfo, void** scanner) {
  SExecTaskInfo* pTaskInfo = (SExecTaskInfo*)tinfo;
  SOperatorInfo* pOperator = pTaskInfo->pRoot;

  while (1) {
    uint8_t type = pOperator->operatorType;
    if (type == QUERY_NODE_PHYSICAL_PLAN_STREAM_SCAN) {
      *scanner = pOperator->info;
      return 0;
    } else {
      ASSERT(pOperator->numOfDownstream == 1);
      pOperator = pOperator->pDownstream[0];
    }
  }
}

int32_t qStreamInput(qTaskInfo_t tinfo, void* pItem) {
  SExecTaskInfo* pTaskInfo = (SExecTaskInfo*)tinfo;
  ASSERT(pTaskInfo->execModel == OPTR_EXEC_MODEL_STREAM);
  taosWriteQitem(pTaskInfo->streamInfo.inputQueue->queue, pItem);
  return 0;
}

void* qExtractReaderFromStreamScanner(void* scanner) {
  SStreamScanInfo* pInfo = scanner;
  return (void*)pInfo->tqReader;
}

const SSchemaWrapper* qExtractSchemaFromStreamScanner(void* scanner) {
  SStreamScanInfo* pInfo = scanner;
  return pInfo->tqReader->pSchemaWrapper;
}

const STqOffset* qExtractStatusFromStreamScanner(void* scanner) {
  SStreamScanInfo* pInfo = scanner;
  return &pInfo->offset;
}

void* qStreamExtractMetaMsg(qTaskInfo_t tinfo) {
  SExecTaskInfo* pTaskInfo = (SExecTaskInfo*)tinfo;
  ASSERT(pTaskInfo->execModel == OPTR_EXEC_MODEL_QUEUE);
  return pTaskInfo->streamInfo.metaBlk;
}

int32_t qStreamExtractOffset(qTaskInfo_t tinfo, STqOffsetVal* pOffset) {
  SExecTaskInfo* pTaskInfo = (SExecTaskInfo*)tinfo;
  ASSERT(pTaskInfo->execModel == OPTR_EXEC_MODEL_QUEUE);
  memcpy(pOffset, &pTaskInfo->streamInfo.lastStatus, sizeof(STqOffsetVal));
  return 0;
}

int32_t qStreamPrepareScan(qTaskInfo_t tinfo, const STqOffsetVal* pOffset) {
  SExecTaskInfo* pTaskInfo = (SExecTaskInfo*)tinfo;
  SOperatorInfo* pOperator = pTaskInfo->pRoot;
  ASSERT(pTaskInfo->execModel == OPTR_EXEC_MODEL_QUEUE);
  pTaskInfo->streamInfo.prepareStatus = *pOffset;
  if (!tOffsetEqual(pOffset, &pTaskInfo->streamInfo.lastStatus)) {
    while (1) {
      uint8_t type = pOperator->operatorType;
      pOperator->status = OP_OPENED;
      if (type == QUERY_NODE_PHYSICAL_PLAN_STREAM_SCAN) {
        SStreamScanInfo* pInfo = pOperator->info;
        if (pOffset->type == TMQ_OFFSET__LOG) {
#if 0
          if (tOffsetEqual(pOffset, &pTaskInfo->streamInfo.lastStatus) &&
              pInfo->tqReader->pWalReader->curVersion != pOffset->version) {
            qError("prepare scan ver %ld actual ver %ld, last %ld", pOffset->version,
                   pInfo->tqReader->pWalReader->curVersion, pTaskInfo->streamInfo.lastStatus.version);
            ASSERT(0);
          }
#endif
          if (tqSeekVer(pInfo->tqReader, pOffset->version + 1) < 0) {
            return -1;
          }
          ASSERT(pInfo->tqReader->pWalReader->curVersion == pOffset->version + 1);
        } else if (pOffset->type == TMQ_OFFSET__SNAPSHOT_DATA) {
          /*pInfo->blockType = STREAM_INPUT__TABLE_SCAN;*/
          int64_t uid = pOffset->uid;
          int64_t ts = pOffset->ts;

          if (uid == 0) {
            if (taosArrayGetSize(pTaskInfo->tableqinfoList.pTableList) != 0) {
              STableKeyInfo* pTableInfo = taosArrayGet(pTaskInfo->tableqinfoList.pTableList, 0);
              uid = pTableInfo->uid;
              ts = INT64_MIN;
            }
          }
          /*if (pTaskInfo->streamInfo.lastStatus.type != TMQ_OFFSET__SNAPSHOT_DATA ||*/
          /*pTaskInfo->streamInfo.lastStatus.uid != uid || pTaskInfo->streamInfo.lastStatus.ts != ts) {*/
          STableScanInfo* pTableScanInfo = pInfo->pTableScanOp->info;
          int32_t         tableSz = taosArrayGetSize(pTaskInfo->tableqinfoList.pTableList);
          bool            found = false;
          for (int32_t i = 0; i < tableSz; i++) {
            STableKeyInfo* pTableInfo = taosArrayGet(pTaskInfo->tableqinfoList.pTableList, i);
            if (pTableInfo->uid == uid) {
              found = true;
              pTableScanInfo->currentTable = i;
              break;
            }
          }

          // TODO after dropping table, table may be not found
          ASSERT(found);

          tsdbSetTableId(pTableScanInfo->dataReader, uid);
          int64_t oldSkey = pTableScanInfo->cond.twindows.skey;
          pTableScanInfo->cond.twindows.skey = ts + 1;
          tsdbReaderReset(pTableScanInfo->dataReader, &pTableScanInfo->cond);
          pTableScanInfo->cond.twindows.skey = oldSkey;
          pTableScanInfo->scanTimes = 0;

          qDebug("tsdb reader offset seek to uid %ld ts %ld, table cur set to %d , all table num %d", uid, ts,
                 pTableScanInfo->currentTable, tableSz);
          /*}*/

        } else {
          ASSERT(0);
        }
        return 0;
      } else {
        ASSERT(pOperator->numOfDownstream == 1);
        pOperator = pOperator->pDownstream[0];
      }
    }
  }
  return 0;
}

#if 0
int32_t qStreamPrepareTsdbScan(qTaskInfo_t tinfo, uint64_t uid, int64_t ts) {
  SExecTaskInfo* pTaskInfo = (SExecTaskInfo*)tinfo;

  if (uid == 0) {
    if (taosArrayGetSize(pTaskInfo->tableqinfoList.pTableList) != 0) {
      STableKeyInfo* pTableInfo = taosArrayGet(pTaskInfo->tableqinfoList.pTableList, 0);
      uid = pTableInfo->uid;
      ts = INT64_MIN;
    }
  }

  return doPrepareScan(pTaskInfo->pRoot, uid, ts);
}

int32_t qGetStreamScanStatus(qTaskInfo_t tinfo, uint64_t* uid, int64_t* ts) {
  SExecTaskInfo* pTaskInfo = (SExecTaskInfo*)tinfo;

  return doGetScanStatus(pTaskInfo->pRoot, uid, ts);
}
#endif