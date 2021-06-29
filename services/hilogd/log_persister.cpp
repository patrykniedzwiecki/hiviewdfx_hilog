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

#include "log_persister.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <climits>
#include <cstdlib>
#include <securec.h>
#include "hilog_common.h"
#include "log_buffer.h"
#include "log_compress.h"
#include "format.h"

namespace OHOS {
namespace HiviewDFX {
using namespace std::literals::chrono_literals;
using namespace std;


static std::list<shared_ptr<LogPersister>> logPersisters;
static std::mutex g_listMutex;

#define SAFE_DELETE(x) \
    do { \
        delete (x); \
        (x) = nullptr; \
    } while (0)

LogPersister::LogPersister(uint32_t id, string path, uint16_t compressType,
                           uint16_t compressAlg, int sleepTime,
                           LogPersisterRotator *rotator, HilogBuffer *_buffer)
    : id(id), path(path), compressType(compressType), compressAlg(compressAlg),
      sleepTime(sleepTime), rotator(rotator)
{
    toExit = false;
    hasExited = false;
    hilogBuffer = _buffer;
    LogCompress = nullptr;
    fdinfo = nullptr;
    buffer = nullptr;
}

LogPersister::~LogPersister()
{
    SAFE_DELETE(rotator);
    SAFE_DELETE(LogCompress);
}

int LogPersister::Init()
{
    int nPos = path.find_last_of('/');
    if (nPos == RET_FAIL) {
        return RET_FAIL;
    }
    mmapPath = path.substr(0, nPos) + "/." + to_string(id);
    if (access(path.substr(0, nPos).c_str(), F_OK) != 0) {
        if (errno == ENOENT) {
            MkDirPath(path.substr(0, nPos).c_str());
        }
    }
    bool hit = false;
    const lock_guard<mutex> lock(g_listMutex);
    for (auto it = logPersisters.begin(); it != logPersisters.end(); ++it)
        if ((*it)->getPath() == path || (*it)->Identify(id)) {
            std::cout << path << std::endl;
            hit = true;
            break;
        }
    if (hit) {
        return RET_FAIL;
    }
    fd = open(mmapPath.c_str(), O_RDWR | O_CREAT | O_EXCL, 0);
    bool restore = false;
    if (fd <= 0) {
        if (errno == EEXIST) {
            cout << "File already exists!" << endl;
            fd = open(mmapPath.c_str(), O_RDWR, 0);
            restore = true;
        }
    } else {
#ifdef DEBUG
        cout << "New log file: " << mmapPath << endl;
#endif
        lseek(fd, MAX_PERSISTER_BUFFER_SIZE - 1, SEEK_SET);
        write(fd, "", 1);
    }
    if (fd < 0) {
#ifdef DEBUG
        cout << "open log file(" << mmapPath << ") failed: " << strerror(errno) << endl;
#endif
        return RET_FAIL;
    }
    fdinfo = fopen((mmapPath + ".info").c_str(), "r+");
    if (fdinfo == nullptr) {
        fdinfo = fopen((mmapPath + ".info").c_str(), "w+");
    }
    if (fdinfo == nullptr) {
#ifdef DEBUG
        cout << "open loginfo file failed: " << strerror(errno) << endl;
#endif
        close(fd);
        return RET_FAIL;
    }
    buffer = (LogPersisterBuffer *)mmap(nullptr, MAX_PERSISTER_BUFFER_SIZE, PROT_READ | PROT_WRITE,
                                        MAP_SHARED, fd, 0);
    close(fd);
    if (buffer == MAP_FAILED) {
#ifdef DEBUG
        cout << "mmap file failed: " << strerror(errno) << endl;
#endif
        fclose(fdinfo);
        return RET_FAIL;
    }
    if (restore == true) {
        int moffset;
        if (fscanf_s(fdinfo, "%04x", &moffset) == -1) {
            return RET_FAIL;
        }
#ifdef DEBUG
        cout << "Recovered persister, Offset=" << moffset << endl;
#endif
        SetBufferOffset(moffset);
        WriteFile();
    } else {
        SetBufferOffset(0);
    }
    logPersisters.push_back(std::static_pointer_cast<LogPersister>(shared_from_this()));
    return 0;
}

void LogPersister::NotifyForNewData()
{
    condVariable.notify_one();
    isNotified = true;
}

int LogPersister::MkDirPath(const char *pMkdir)
{
    int isCreate = mkdir(pMkdir, S_IRUSR | S_IWUSR | S_IXUSR | S_IRWXG | S_IRWXO);
    if (!isCreate)
        cout << "create path:" << pMkdir << endl;
    return isCreate;
}

void LogPersister::SetBufferOffset(int off)
{
    buffer->offset = off;
    fseek(fdinfo, 0, SEEK_SET);
    fprintf(fdinfo, "%04x\n", off);
}

int GenPersistLogHeader(HilogData *data, list<string>& persistList)
{
    char buffer[MAX_LOG_LEN];
    HilogShowFormatBuffer showBuffer;
    showBuffer.level = data->level;
    showBuffer.pid = data->pid;
    showBuffer.tid = data->tid;
    showBuffer.domain = data->domain;
    showBuffer.tv_sec = data->tv_sec;
    showBuffer.tv_nsec = data->tv_nsec;
    showBuffer.data = data->tag;
    int offset = data->tag_len;
    
    static char *dataCopy;
    memcpy_s(&dataCopy, data->len, &data->content, data->len);
    char *dataBegin = dataCopy;
    char *dataPos = dataCopy;
  
    while (*dataPos != 0) {
        if (*dataPos == '\n') {
            if (dataPos != dataBegin) {
                *dataPos = 0;
                showBuffer.tag_len = offset;
                HilogShowBuffer(buffer, MAX_LOG_LEN * 2, showBuffer, OFF_SHOWFORMAT);
                persistList.push_back(buffer);
                offset += dataPos - dataBegin + 1;
            } else {
                offset++;
            }
            dataBegin = dataPos + 1;
        }
        dataPos++;
    }
    if (dataPos != dataBegin) {
        showBuffer.tag_len = offset;
        HilogShowBuffer(buffer, MAX_LOG_LEN * 2, showBuffer, OFF_SHOWFORMAT);
        persistList.push_back(buffer);
    }
    return persistList.size();
}

bool LogPersister::writeUnCompressedBuffer(HilogData *data)
{
    int listSize = persistList.size();

    if (persistList.empty()) {
        listSize = GenPersistLogHeader(data, persistList);    
    }
    while (listSize--) {
        string header = persistList.front();
        uint16_t headerLen = header.length();
        uint16_t size = headerLen + 1;
        uint16_t orig_offset = buffer->offset;
        int r = 0;
        if (buffer->offset + size > MAX_PERSISTER_BUFFER_SIZE)
            return false;
        
        r = memcpy_s(buffer->content + buffer->offset, MAX_PERSISTER_BUFFER_SIZE - buffer->offset,
            header.c_str(), headerLen);
        if (r != 0) {
            SetBufferOffset(orig_offset);
            return true;
        }
        persistList.pop_front();
        SetBufferOffset(buffer->offset + headerLen);
        buffer->content[buffer->offset] = '\n';
        SetBufferOffset(buffer->offset + 1);
    }
    return true;
}

int LogPersister::WriteData(HilogData *data)
{
    if (data == nullptr)
        return -1;
    if (writeUnCompressedBuffer(data))
        return 0;
    switch (compressAlg) {
        case COMPRESS_TYPE_OFF:
            WriteFile();
            break;
        case COMPRESS_TYPE_ZLIB: {
            LogCompress = new ZlibCompress();
#ifdef DEBUG
            cout << buffer->content << endl;
#endif
            LogCompress->Compress((Bytef *)buffer->content, buffer->offset);
            rotator->Input((char *)LogCompress->zdata, LogCompress->zdlen);
            rotator->FinishInput();
            SetBufferOffset(0);
            }
            break;
#ifdef USING_ZSTD_COMPRESS
        case COMPRESS_TYPE_ZSTD:  {
            LogCompress = new ZstdCompress();
            LogCompress->Compress((Bytef *)buffer->content, buffer->offset);
            rotator->Input((char *)LogCompress->zdata, LogCompress->zdlen);
            rotator->FinishInput();
            SetBufferOffset(0);
        }
            break;
#endif // #ifdef USING_ZSTD_COMPRESS
        default:
            break;
    }
    return writeUnCompressedBuffer(data) ? 0 : -1;
}

void LogPersister::Start()
{
    auto newThread =
        thread(&LogPersister::ThreadFunc, static_pointer_cast<LogPersister>(shared_from_this()));
    newThread.detach();
    return;
}

inline void LogPersister::WriteFile()
{
    if (buffer->offset == 0) return;
    if (compressAlg == 0) {
        rotator->Input(buffer->content, buffer->offset);
    }
    SetBufferOffset(0);
}

int LogPersister::ThreadFunc()
{
    std::thread::id tid = std::this_thread::get_id();
    cout << __func__ << " " << tid << endl;
    while (true) {
        if (toExit) {
            break;
        }
        if (!hilogBuffer->Query(shared_from_this())) {
            unique_lock<mutex> lk(cvMutex);
            if (condVariable.wait_for(lk, sleepTime * 1s) ==
                cv_status::timeout) {
                if (toExit) {
                    break;
                }
                WriteFile();
            }
        }
        cout << "running! " << compressAlg << endl;
    }
    WriteFile();
    {
        std::lock_guard<mutex> guard(mutexForhasExited);
        hasExited = true;
    }
    cvhasExited.notify_all();
    return 0;
}

int LogPersister::Query(uint16_t logType, list<LogPersistQueryResult> &results)
{
    std::lock_guard<mutex> guard(g_listMutex);
    cout << "Persister.Query: logType " << logType << endl;
    for (auto it = logPersisters.begin(); it != logPersisters.end(); ++it) {
        cout << "Persister.Query: (*it)->queryCondition.types "
             << (*it)->queryCondition.types << endl;
        if (((*it)->queryCondition.types & logType) != 0) {
            LogPersistQueryResult response;
            response.logType = (*it)->queryCondition.types;
            (*it)->FillInfo(&response);
            results.push_back(response);
        }
    }
    return 0;
}

void LogPersister::FillInfo(LogPersistQueryResult *response)
{
    response->jobId = id;
    if (strcpy_s(response->filePath, FILE_PATH_MAX_LEN, path.c_str())) {
        return;
    }
    response->compressType = compressType;
    response->compressAlg = compressAlg;
    rotator->FillInfo(&response->fileSize, &response->fileNum);
    return;
}

int LogPersister::Kill(const uint32_t id)
{
    bool found = false;
    std::lock_guard<mutex> guard(g_listMutex);
    for (auto it = logPersisters.begin(); it != logPersisters.end(); ) {
        if ((*it)->Identify(id)) {
            cout << "find a persister" << endl;
            (*it)->Exit();
            it = logPersisters.erase(it);
            found = true;
        } else {
            ++it;
        }
    }
    return found ? 0 : -1;
}

bool LogPersister::isExited()
{
    return hasExited;
}

void LogPersister::Exit()
{
    toExit = true;
    condVariable.notify_all();
    unique_lock<mutex> lk(mutexForhasExited);
    if (!isExited()) {
        cvhasExited.wait(lk);
    }
    delete rotator;
    this->rotator = nullptr;
    munmap(buffer, MAX_PERSISTER_BUFFER_SIZE);
    cout << "removed mmap file" << endl;
    remove(mmapPath.c_str());
    remove((mmapPath + ".info").c_str());
    fclose(fdinfo);
    return;
}
bool LogPersister::Identify(uint32_t id)
{
    return this->id == id;
}

string LogPersister::getPath()
{
    return path;
}

uint8_t LogPersister::getType() const
{
    return TYPE_PERSISTER;
}
} // namespace HiviewDFX
} // namespace OHOS
