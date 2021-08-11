/*
 * Copyright (c) 2021 Huawei Device Co., Ltd.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef HILOG_COMMON_H
#define HILOG_COMMON_H

#include <cstdint>

#ifdef HILOG_USE_MUSL
#define SOCKET_FILE_DIR "/dev/unix/socket/"
#else
#define SOCKET_FILE_DIR "/dev/socket/"
#endif
#define INPUT_SOCKET_NAME "hilogInput"
#define INPUT_SOCKET SOCKET_FILE_DIR INPUT_SOCKET_NAME
#define CONTROL_SOCKET_NAME "hilogControl"
#define CONTROL_SOCKET SOCKET_FILE_DIR CONTROL_SOCKET_NAME
#define HLIOG_FILE_DIR "/data/log/hilog/"

#define SENDIDN 0    // hilogd: reached end of log;  hilogtool: exit log reading
#define SENDIDA 1    // hilogd & hilogtool: normal log reading
#define SENDIDS 2    // hilogd: notify for new data; hilogtool: block and wait for new data
#define MULARGS 5
#define MAX_LOG_LEN 1024  /* maximum length of a log, include '\0' */
#define MAX_TAG_LEN 32  /* log tag size, include '\0' */
#define MAX_DOMAINS 5
#define MAX_TAGS 10
#define MAX_PIDS 5
#define RET_SUCCESS 0
#define RET_FAIL (-1)
#define IS_ONE(number, n) ((number>>n)&0x01)
#define ONE_KB (1UL<<10)
#define ONE_MB (1UL<<20)
#define ONE_GB (1UL<<30)
#define ONE_TB (1ULL<<40)

#define DOMAIN_NUMBER_BASE (16)

/*
 * header of log message from libhilog to hilogd
 */
using HilogMsg = struct __attribute__((__packed__)) {
    uint16_t len;
    uint16_t version : 3;
    uint16_t type : 4;  /* APP,CORE,INIT,SEC etc */
    uint16_t level : 3;
    uint16_t tag_len : 6; /* include '\0' */
    uint32_t tv_sec;
    uint32_t tv_nsec;
    uint32_t pid;
    uint32_t tid;
    uint32_t domain;
    char tag[]; /* shall be end with '\0' */
};

using HilogShowFormatBuffer = struct {
    uint16_t length;
    uint16_t level;
    uint16_t type;
    uint16_t tag_len;
    uint32_t pid;
    uint32_t tid;
    uint32_t domain;
    uint32_t tv_sec;
    uint32_t tv_nsec;
    char* data;
};

#define CONTENT_LEN(pMsg) (pMsg->len - sizeof(HilogMsg) - pMsg->tag_len) /* include '\0' */
#define CONTENT_PTR(pMsg) (pMsg->tag + pMsg->tag_len)

#define likely(x)      __builtin_expect(!!(x), 1)
#define unlikely(x)    __builtin_expect(!!(x), 0)

/*-*********************************************
 *  Error codes list
 *-*********************************************
 *  Error codes _values_ are pinned down.
 **********************************************/
typedef enum {
    ERR_LOG_LEVEL_INVALID  = -1,
    ERR_LOG_TYPE_INVALID = -2,
    ERR_QUERY_LEVEL_INVALID = -3,
    ERR_QUERY_TAG_INVALID = -4,
    ERR_QUERY_PID_INVALID = -5,
    ERR_QUERY_TYPE_INVALID = -6,
    ERR_BUFF_SIZE_INVALID = -7,
    ERR_BUFF_SIZE_EXP = -8,
    ERR_LOG_PERSIST_FILE_SIZE_INVALID = -9,
    ERR_LOG_PERSIST_FILE_NAME_INVALID = -10,
    ERR_LOG_PERSIST_FILE_PATH_EXP = -11,
    ERR_LOG_PERSIST_COMPRESS_INIT_FAIL = -12,
    ERR_LOG_PERSIST_FILE_OPEN_FAIL = -13,
    ERR_LOG_PERSIST_MMAP_FAIL = -14,
    ERR_LOG_PERSIST_JOBID_FAIL = -15,
    ERR_DOMAIN_INVALID = -16,
    ERR_MEM_ALLOC_FAIL = -17,
    ERR_MSG_LEN_INVALID = -18,
    ERR_PROPERTY_VALUE_INVALID = -19,
    ERR_LOG_CONTENT_NULL = -20,
    ERR_COMMAND_NOT_FOUND = -21,
    ERR_FORMAT_INVALID = -22
} ErrorCode;
#endif /* HILOG_COMMON_H */
