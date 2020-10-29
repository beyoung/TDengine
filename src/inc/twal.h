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
#ifndef _TD_WAL_H_
#define _TD_WAL_H_

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  TAOS_WAL_NOLOG = 0,
  TAOS_WAL_WRITE = 1,
  TAOS_WAL_FSYNC = 2
} EWalType;

typedef struct {
  int8_t   msgType;
  int8_t   reserved[3];
  int32_t  len;
  uint64_t version;
  uint32_t signature;
  uint32_t cksum;
  char     cont[];
} SWalHead;

typedef struct {
  int32_t vgId;
  int32_t fsyncPeriod;  // millisecond
  int8_t  walLevel;     // wal level
  int8_t  wals;         // number of WAL files;
  int8_t  keep;         // keep the wal file when closed
  int8_t  reserved[5];
} SWalCfg;

typedef void* twalh;  // WAL HANDLE
typedef int (*FWalWrite)(void *ahandle, void *pHead, int type);

int32_t walInit();
void    walCleanUp();

twalh   walOpen(char *path, SWalCfg *pCfg);
int     walAlter(twalh pWal, SWalCfg *pCfg);
void    walClose(twalh);
int     walRenew(twalh);
int     walWrite(twalh, SWalHead *);
void    walFsync(twalh);
int     walRestore(twalh, void *pVnode, FWalWrite writeFp);
int     walGetWalFile(twalh, char *name, uint32_t *index);
int64_t walGetVersion(twalh);

#ifdef __cplusplus
}
#endif

#endif  // _TD_WAL_H_
