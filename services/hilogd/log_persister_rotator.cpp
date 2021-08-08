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
#include "log_persister_rotator.h"

#include <cstdio>
#include <fstream>
#include <iostream>
#include <sstream>
#include <unistd.h>
#include <sys/stat.h>

namespace OHOS {
namespace HiviewDFX {
using namespace std;

LogPersisterRotator::LogPersisterRotator(string path, uint32_t fileSize, uint32_t fileNum, string suffix)
    : fileNum(fileNum), fileSize(fileSize), fileName(path), fileSuffix(suffix)
{
    index = -1;
    needRotate = true;
}

LogPersisterRotator::~LogPersisterRotator()
{
    if (fdinfo != nullptr) {
        fclose(fdinfo);
    }
}

int LogPersisterRotator::Init()
{
    int nPos = fileName.find_last_of('/');
    std::string mmapPath = fileName.substr(0, nPos) + "/." + ANXILLARY_FILE_NAME + to_string(id);
    if (access(fileName.substr(0, nPos).c_str(), F_OK) != 0) {
        if (errno == ENOENT) {
            mkdir(fileName.substr(0, nPos).c_str(), S_IRUSR | S_IWUSR | S_IXUSR | S_IRWXG | S_IRWXO);
        }
    }
    fdinfo = fopen((mmapPath + ".info").c_str(), "r+");
    if (fdinfo == nullptr) return RET_FAIL;
    return RET_SUCCESS;
}

int LogPersisterRotator::Input(const char *buf, uint32_t length)
{
    cout << __func__ << " " << fileName << " " << index
        << " " << length  << " need: " << needRotate << endl;
    if (length <= 0 || buf == nullptr) return -1;
    if (needRotate) {
        output.close();
        Rotate();
        needRotate = false;
    }
    output.write(buf, length);
    output.flush();
    return 0;
}

void LogPersisterRotator::InternalRotate()
{
    stringstream ss;
    ss << fileName << ".";
    int pos = ss.tellp();
    ss << 0 << fileSuffix;
    remove(ss.str().c_str());

    for (uint32_t i = 1; i < fileNum; ++i) {
        ss.seekp(pos);
        ss << (i - 1) << fileSuffix;
        string newName = ss.str();
        ss.seekp(pos);
        ss << i << fileSuffix;
        string oldName = ss.str();
        cout << "OLD NAME " << oldName << " NEW NAME " << newName << endl;
        rename(oldName.c_str(), newName.c_str());
    }
    output.open(ss.str(), ios::out | ios::trunc);
}

void LogPersisterRotator::Rotate()
{
    cout << __func__ << endl;
    if (index >= (int)(fileNum - 1)) {
        InternalRotate();
    } else {
        index += 1;
        stringstream ss;
        ss << fileName << "." << index << fileSuffix;
        cout << "THE FILE NAME !!!!!!! " << ss.str() << endl;
        output.open(ss.str(), ios::out | ios::trunc);
    }
    fseek(fdinfo, 0, SEEK_SET);
    fwrite(&index, sizeof(uint8_t), 1, fdinfo);
    fsync(fileno(fdinfo));
}

void LogPersisterRotator::FillInfo(uint32_t *size, uint32_t *num)
{
    *size = fileSize;
    *num = fileNum;
}

void LogPersisterRotator::FinishInput()
{
    needRotate = true;
}

void LogPersisterRotator::SetIndex(int pIndex)
{
    index = pIndex;
}

void LogPersisterRotator::SetId(uint32_t pId)
{
    id = pId;
}
} // namespace HiviewDFX
} // namespace OHOS