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

#include "log_buffer.h"

#include <cstring>
#include "hilog_common.h"
#include "flow_control_init.h"
#include "log_time_stamp.h"
namespace OHOS {
namespace HiviewDFX {
using namespace std;

const float DROP_RATIO = 0.05;
static int g_maxBufferSize = 4194304;
static int g_maxBufferSizeByType[LOG_TYPE_MAX] = {262144, 262144, 262144, 262144, 262144};
const int DOMAIN_STRICT_MASK = 0xd000000;
const int DOMAIN_FUZZY_MASK = 0xdffff;
const int DOMAIN_MODULE_BITS = 8;
const int MAX_TIME_DIFF = 5;

HilogBuffer::HilogBuffer()
{
    size = 0;
    for (int i = 0; i < LOG_TYPE_MAX; i++) {
        sizeByType[i] = 0;
        cacheLenByType[i] = 0;
        printLenByType[i] = 0;
        droppedByType[i] = 0;
    }
}

HilogBuffer::~HilogBuffer() {}


size_t HilogBuffer::Insert(const HilogMsg& msg)
{
    size_t eleSize = CONTENT_LEN((&msg)); /* include '\0' */

    if (unlikely(msg.tag_len > MAX_TAG_LEN || msg.tag_len == 0 || eleSize > MAX_LOG_LEN || eleSize <= 0)) {
        return 0;
    }

    std::list<HilogData> &msgList = (msg.type == LOG_KMSG) ? hilogKlogList : hilogDataList;
    // Delete old entries when full
    if (eleSize + sizeByType[msg.type] >= (size_t)g_maxBufferSizeByType[msg.type]) {
        hilogBufferMutex.lock();
        // Drop 5% of maximum log when full
        std::list<HilogData>::iterator it = msgList.begin();
        while (sizeByType[msg.type] > g_maxBufferSizeByType[msg.type] * (1 - DROP_RATIO) &&
            it != msgList.end()) {
            if ((*it).type != msg.type) {    // Only remove old logs of the same type
                ++it;
                continue;
            }
            logReaderListMutex.lock_shared();
            for (auto &itr :logReaderList) {
                auto reader = itr.lock();
                if (reader == nullptr) {
                    continue;
                }
                if (reader->readPos == it) {
                    reader->readPos = std::next(it);
                }
                if (reader->lastPos == it) {
                    reader->lastPos = std::next(it);
                }
            }
            logReaderListMutex.unlock_shared();
            size_t cLen = it->len - it->tag_len;
            size -= cLen;
            sizeByType[(*it).type] -= cLen;
            it = msgList.erase(it);
        }

        // Re-confirm if enough elements has been removed
        if (sizeByType[msg.type] >= (size_t)g_maxBufferSizeByType[msg.type]) {
            std::cout << "Failed to clean old logs." << std::endl;
        }
        hilogBufferMutex.unlock();
    }

    // Insert new log into HilogBuffer
    std::list<HilogData>::reverse_iterator rit = msgList.rbegin();
    std::list<HilogData>::reverse_iterator ritEnd = msgList.rend(); 
    LogTimeStamp msgTimeStamp(msg.tv_sec, msg.tv_nsec);
    LogTimeStamp ritTimeStamp(rit->tv_sec, rit->tv_nsec);
    LogTimeStamp measureTimeStamp(ritEnd->tv_sec, ritEnd->tv_nsec);
    if (msgTimeStamp >= ritTimeStamp || msgTimeStamp < measureTimeStamp ||
        (ritTimeStamp -= msgTimeStamp) > LogTimeStamp(MAX_TIME_DIFF)) {
        msgList.emplace_back(msg);
    } else {
        // Find the place with right timestamp
        ++rit;
        ritTimeStamp.SetTimeStamp(rit->tv_sec, rit->tv_nsec);
        for (; rit != msgList.rend() && (msgTimeStamp < ritTimeStamp); ++rit) {
            ritTimeStamp.SetTimeStamp(rit->tv_sec, rit->tv_nsec);
        }
        msgList.emplace(rit.base(), msg);
    }
    // Update current size of HilogBuffer
    size += eleSize;
    sizeByType[msg.type] += eleSize;
    cacheLenByType[msg.type] += eleSize;
    if (cacheLenByDomain.count(msg.domain) == 0) {
        cacheLenByDomain.insert(pair<uint32_t, uint64_t>(msg.domain, eleSize));
    } else {
        cacheLenByDomain[msg.domain] += eleSize;
    }
    return eleSize;
}


bool HilogBuffer::Query(std::shared_ptr<LogReader> reader)
{
    uint16_t qTypes = reader->queryCondition.types;
    std::list<HilogData> &msgList = (qTypes == (0b01 << LOG_KMSG)) ? hilogKlogList : hilogDataList;
    hilogBufferMutex.lock_shared();
    if (reader->GetReload()) {
        reader->readPos = msgList.begin();
        reader->lastPos = msgList.begin();
        reader->SetReload(false);
    }

    if (reader->isNotified) {
        if (reader->readPos == msgList.end()) {
            reader->readPos = std::next(reader->lastPos);
        }
    }
    while (reader->readPos != msgList.end()) {
        reader->lastPos = reader->readPos;
        if (ConditionMatch(reader)) {
            reader->SetSendId(SENDIDA);
            reader->WriteData(*(reader->readPos));
            printLenByType[reader->readPos->type] += strlen(reader->readPos->content);
            if (printLenByDomain.count(reader->readPos->domain) == 0) {
                printLenByDomain.insert(pair<uint32_t, uint64_t>(reader->readPos->domain,
                    strlen(reader->readPos->content)));
            } else {
                printLenByDomain[reader->readPos->domain] += strlen(reader->readPos->content);
            }
            reader->readPos++;
            hilogBufferMutex.unlock_shared();
            return true;
        }
        reader->readPos++;
    }
    reader->isNotified = false;
    ReturnNoLog(reader);
    hilogBufferMutex.unlock_shared();
    return false;
}

size_t HilogBuffer::Delete(uint16_t logType)
{
    std::list<HilogData> &msgList = (logType == (0b01 << LOG_KMSG)) ? hilogKlogList : hilogDataList;
    if (logType >= LOG_TYPE_MAX) {
        return ERR_LOG_TYPE_INVALID;
    }
    size_t sum = 0;
    hilogBufferMutex.lock();
    std::list<HilogData>::iterator it = msgList.begin();

    // Delete logs corresponding to queryCondition
    while (it != msgList.end()) {
        // Only remove old logs of the same type
        if ((*it).type != logType) {
            ++it;
            continue;
        }
        // Delete corresponding logs
        logReaderListMutex.lock_shared();
        for (auto &itr :logReaderList) {
            auto reader = itr.lock();
            if (reader == nullptr) {
                continue;
            }
            if (reader->readPos == it) {
                reader->readPos = std::next(it);
            }
            if (reader->lastPos == it) {
                reader->lastPos = std::next(it);
            }
        }
        logReaderListMutex.unlock_shared();

        size_t cLen = it->len - it->tag_len;
        sum += cLen;
        sizeByType[(*it).type] -= cLen;
        size -= cLen;
        it = msgList.erase(it);
    }

    hilogBufferMutex.unlock();
    return sum;
}

void HilogBuffer::AddLogReader(std::weak_ptr<LogReader> reader)
{
    std::list<HilogData> &msgList = (reader.lock()->queryCondition.types ==
        (0b01 << LOG_KMSG)) ? hilogKlogList : hilogDataList;
    logReaderListMutex.lock();
    // If reader not in logReaderList
    logReaderList.push_back(reader);
    reader.lock()->lastPos = msgList.end();
    logReaderListMutex.unlock();
}

void HilogBuffer::RemoveLogReader(std::shared_ptr<LogReader> reader)
{
    logReaderListMutex.lock();
    const auto findIter = std::find_if(logReaderList.begin(), logReaderList.end(),
        [&reader](const std::weak_ptr<LogReader>& ptr0) {
        return ptr0.lock() == reader;
    });
    if (findIter != logReaderList.end()) {
        logReaderList.erase(findIter);
    }
    logReaderListMutex.unlock();
}

bool HilogBuffer::Query(LogReader* reader)
{
    return Query(std::shared_ptr<LogReader>(reader));
}

size_t HilogBuffer::GetBuffLen(uint16_t logType)
{
    if (logType >= LOG_TYPE_MAX) {
        return ERR_LOG_TYPE_INVALID;
    }
    uint64_t buffSize = g_maxBufferSizeByType[logType];
    return buffSize;
}

size_t HilogBuffer::SetBuffLen(uint16_t logType, uint64_t buffSize)
{
    if (logType >= LOG_TYPE_MAX) {
        return ERR_LOG_TYPE_INVALID;
    }
    if (buffSize <= 0 || buffSize > MAX_BUFFER_SIZE) {
        return ERR_BUFF_SIZE_INVALID;
    }
    g_maxBufferSizeByType[logType] = buffSize;
    g_maxBufferSize += (buffSize - sizeByType[logType]);
    return buffSize;
}

int32_t HilogBuffer::GetStatisticInfoByLog(uint16_t logType, uint64_t& printLen, uint64_t& cacheLen, int32_t& dropped)
{
    if (logType >= LOG_TYPE_MAX) {
        return ERR_LOG_TYPE_INVALID;
    }
    printLen = printLenByType[logType];
    cacheLen = cacheLenByType[logType];
    dropped = GetDroppedByType(logType);
    return 0;
}

int32_t HilogBuffer::GetStatisticInfoByDomain(uint32_t domain, uint64_t& printLen, uint64_t& cacheLen,
    int32_t& dropped)
{
    printLen = printLenByDomain[domain];
    cacheLen = cacheLenByDomain[domain];
    dropped = GetDroppedByDomain(domain);
    return 0;
}

int32_t HilogBuffer::ClearStatisticInfoByLog(uint16_t logType)
{
    if (logType >= LOG_TYPE_MAX) {
        return ERR_LOG_TYPE_INVALID;
    }
    ClearDroppedByType();
    printLenByType[logType] = 0;
    cacheLenByType[logType] = 0;
    droppedByType[logType] = 0;
    return 0;
}

int32_t HilogBuffer::ClearStatisticInfoByDomain(uint32_t domain)
{
    ClearDroppedByDomain();
    printLenByDomain[domain] = 0;
    cacheLenByDomain[domain] = 0;
    droppedByDomain[domain] = 0;
    return 0;
}

bool HilogBuffer::ConditionMatch(std::shared_ptr<LogReader> reader)
{
    /* domain patterns:
     * strict mode: 0xdxxxxxx   (full)
     * fuzzy mode: 0xdxxxx      (using last 2 digits of full domain as mask)
     */
    if (((static_cast<uint8_t>((0b01 << (reader->readPos->type)) & (reader->queryCondition.types)) == 0) ||
        (static_cast<uint8_t>((0b01 << (reader->readPos->level)) & (reader->queryCondition.levels)) == 0)))
        return false;

    int ret = 0;
    if (reader->queryCondition.nPid > 0) {
        for (int i = 0; i < reader->queryCondition.nPid; i++) {
            if (reader->readPos->pid == reader->queryCondition.pids[i]) {
                ret = 1;
                break;
            }
        }
        if (ret == 0) return false;
        ret = 0;
    }
    if (reader->queryCondition.nDomain > 0) {
        for (int i = 0; i < reader->queryCondition.nDomain; i++) {
            uint32_t domains = reader->queryCondition.domains[i];
            if (!((domains >= DOMAIN_STRICT_MASK && domains != reader->readPos->domain) ||
                (domains <= DOMAIN_FUZZY_MASK && domains != (reader->readPos->domain >> DOMAIN_MODULE_BITS)))) {
                ret = 1;
                break;
            }
        }
        if (ret == 0) return false;
        ret = 0;
    }
    if (reader->queryCondition.nTag > 0) {
        for (int i = 0; i < reader->queryCondition.nTag; i++) {
            if (reader->readPos->tag == reader->queryCondition.tags[i]) {
                ret = 1;
                break;
            }
        }
        if (ret == 0) return false;
        ret = 0;
    }

    // exclusion
    if (reader->queryCondition.nNoPid > 0) {
        for (int i = 0; i < reader->queryCondition.nNoPid; i++) {
            if (reader->readPos->pid == reader->queryCondition.noPids[i]) return false;
        }
    }
    if (reader->queryCondition.nNoDomain != 0) {
        for (int i = 0; i < reader->queryCondition.nNoDomain; i++) {
            uint32_t noDomains = reader->queryCondition.noDomains[i];
            if (((noDomains >= DOMAIN_STRICT_MASK && noDomains == reader->readPos->domain) ||
                (noDomains <= DOMAIN_FUZZY_MASK && noDomains == (reader->readPos->domain >> DOMAIN_MODULE_BITS))))
                return false;
        }
    }
    if (reader->queryCondition.nNoTag > 0) {
        for (int i = 0; i < reader->queryCondition.nNoTag; i++) {
            if (reader->readPos->tag == reader->queryCondition.noTags[i]) return false;
        }
    }
    if ((static_cast<uint8_t>((0b01 << (reader->readPos->type)) & (reader->queryCondition.noTypes)) != 0) ||
        (static_cast<uint8_t>((0b01 << (reader->readPos->level)) & (reader->queryCondition.noLevels)) != 0)) {
        return false;
    }
    return true;
}

void HilogBuffer::ReturnNoLog(std::shared_ptr<LogReader> reader)
{
    reader->SetSendId(SENDIDN);
    reader->WriteData(std::nullopt);
}

void HilogBuffer::GetBufferLock()
{
    hilogBufferMutex.lock();
}

void HilogBuffer::ReleaseBufferLock()
{
    hilogBufferMutex.unlock();
}
} // namespace HiviewDFX
} // namespace OHOS
