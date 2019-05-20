// Copyright 2019 ByteDance Inc. or its affiliates. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
// =============================================================================

#include <cstring>
#include <memory>
#include <thread>
#include <chrono>
#include <cuda_runtime.h>

#include "logging.h"
#include "operations.h"
#include "global.h"

namespace byteps {
namespace common {

void FinishOrProceed(std::shared_ptr<TensorTableEntry> task) {
    BPS_CHECK_GE(task->queue_list.size(), 1);
    auto this_op = task->queue_list[0];
    task->queue_list.erase(task->queue_list.begin());
    if (task->queue_list.size() > 0) {
        BPS_LOG(TRACE) << "Rank=" << BytePSGlobal::GetRank()
                       << " finishes " << LogStrings[this_op] << ", tensor: " << task->tensor_name
                       << ", key=" << task->key << "; Passing to the next queue.";
        BytePSGlobal::GetScheduledQueue(task->queue_list[0])->addTask(task);
    } else {
        BPS_CHECK(task->counter_ptr) << task->tensor_name << " counter_ptr is null";
        int v = task->counter_ptr.get()->fetch_add(1);
        if (v == (task->total_partnum-1)) {
            BPS_LOG(TRACE) << "Rank=" << BytePSGlobal::GetRank()
                           << "Finish processing tensor: " << task->tensor_name;
            task->callback(Status::OK());
        }
    }
    return;
}

bool RunCoordinateLoopOnce(QueueType this_op) {
    auto q = BytePSGlobal::GetScheduledQueue(this_op);
    auto task = q->getTask();
    if (task){
        BPS_CHECK(!IsRoot()) << "only non-root device should enter COORDINATE loop";

        int root = GetRoot();
        int rank = GetMyLocalRank();
        int key  = task->key;

        // first send to next queue and then broadcast signal
        // to guarantee the entry is available when getTask(key) at Reduce/Broadcast thread
        FinishOrProceed(task);

        struct BytePSCommMsg msg = { rank,
                                     (this_op == COORDINATE_REDUCE) ? REDUCE_READY : BCAST_READY,
                                     key };
        BytePSGlobal::GetComm()->sendSignal(root, &msg, sizeof(BytePSCommMsg));

        BPS_LOG(TRACE) << task->tensor_name << " send coordinate info: "
                       << "root=" << root
                       << ", rank=" << rank
                       << ", key=" << key;

        q->reportFinish(task->len);

    } else {
        std::this_thread::sleep_for(std::chrono::nanoseconds(1000));
    }
    return true;
}

bool RunRootNcclLoopOnce() {
    auto nccl_stream = BytePSGlobal::GetNcclStream();
    auto nccl_comm = BytePSGlobal::GetNcclComm();
    int root = GetRoot();
    int rank = GetMyLocalRank();
    BPS_CHECK_EQ(rank, root);

    QueueType nccl_ops[] = { REDUCE, BROADCAST };

    auto nccl_entry = std::make_shared<NcclGroupEntry>(); 
    auto &tasks = nccl_entry->tasks;
    auto &queues = nccl_entry->queues;

    NCCLCHECK(ncclGroupStart());
    for (auto this_op : nccl_ops) {
        auto q = BytePSGlobal::GetScheduledQueue(this_op);
        for (int i = 0; i < BytePSGlobal::GetNcclGroupSize(); i++) {
            auto task = q->getTask();
            if (!task) { break; }
            tasks.push_back(task);
            queues.push_back(q);

            auto tensor = (this_op == REDUCE) ? task->tensor : task->output;
            BPS_CHECK(tensor);
            
            int key  = task->key;
            auto len = task->len;
            auto offset = task->offset;

            if (task->device != CPU_DEVICE_ID) { // GPU
                BPS_CHECK(tensor->data());
                BPS_CHECK_EQ(0, tensor->size() % tensor->shape().num_elements());

                auto nccl_dtype = getNcclDataType(tensor->dtype());
                auto unit_len = tensor->size() / tensor->shape().num_elements();

                BPS_LOG(TRACE) << task->tensor_name << " calling NCCL "
                               << LogStrings[this_op]
                               << " (rank=" << rank
                               << ") key=" << key
                               << ", elements=" << len/unit_len
                               << ", device=" << task->device;

                if (BytePSGlobal::GetLocalSize() > 1) {
                    // notify non-root devices
                    struct BytePSCommMsg msg = { rank,
                                                 (this_op == REDUCE) ? DO_REDUCE : DO_BROADCAST,
                                                 key };
                    BytePSGlobal::GetComm()->broadcastSignal(rank, &msg,
                                                             sizeof(BytePSCommMsg));

                    if (this_op == REDUCE) {
                        NCCLCHECK(ncclReduce((const void*) (tensor->data()+offset),
                                             (void*) (tensor->data()+offset),
                                             len/unit_len, nccl_dtype, ncclSum, root,
                                             *nccl_comm, *nccl_stream));
                    }
                    else {
                        NCCLCHECK(ncclBroadcast((const void*) (tensor->data()+offset),
                                                (void*) (tensor->data()+offset),
                                                len/unit_len, nccl_dtype, root,
                                                *nccl_comm, *nccl_stream)); 
                    }
                }
            }
        }
    }
    if (tasks.size()) {
        struct BytePSCommMsg msg = { rank, DO_GROUP, 0 };
        BytePSGlobal::GetComm()->broadcastSignal(rank, &msg, sizeof(BytePSCommMsg));
        BPS_LOG(TRACE) << "NCCL Group size=" << tasks.size() << " rank=" << rank;
        NCCLCHECK(ncclGroupEnd());
        CUDA_CALL(cudaEventCreateWithFlags(&(nccl_entry->cuda_event), cudaEventBlockingSync | cudaEventDisableTiming));
        CUDA_CALL(cudaEventRecord(nccl_entry->cuda_event, *nccl_stream));
        BytePSGlobal::EnqueueNcclGroup(nccl_entry);
    }
    else {
        NCCLCHECK(ncclGroupEnd());
        std::this_thread::sleep_for(std::chrono::nanoseconds(1000));
    }

    return true;
}

bool RunNonRootNcclLoopOnce() {
    auto nccl_stream = BytePSGlobal::GetNcclStream();
    auto nccl_comm = BytePSGlobal::GetNcclComm();
    int root = GetRoot();
    int rank = GetMyLocalRank();
    BPS_CHECK_NE(rank, root);

    auto nccl_entry = std::make_shared<NcclGroupEntry>(); 
    auto &tasks = nccl_entry->tasks;
    int src;
    struct BytePSCommMsg msg = {};

    NCCLCHECK(ncclGroupStart());
    while (1) {
        BytePSGlobal::GetComm()->recvSignal(&src, &msg, sizeof(BytePSCommMsg));
        BPS_CHECK_EQ(src, root) << msg.src << ", " << root; // should only receive from root
        if (msg.signal == DO_GROUP) { break; }
        QueueType this_op = REDUCE;
        if (msg.signal == DO_BROADCAST) {
            this_op = BROADCAST;
        }
        else {
            BPS_CHECK_EQ(msg.signal, DO_REDUCE) << msg.signal << ", " << DO_REDUCE;
        }

        int key = msg.key;
        BPS_LOG(TRACE) << "rank=" << rank << " receving BROADCAST key=" << key;

        auto q = BytePSGlobal::GetScheduledQueue(this_op);
        auto task = q->getTask(key);
        BPS_CHECK(task);
        BPS_CHECK_EQ(task->queue_list.size(), 1)
            << "BROADCAST should be the last op, "
            << "remain queue_list size: " << task->queue_list.size()
            << ", local_rank=" << rank;

        tasks.push_back(task);

        auto tensor = (this_op == REDUCE) ? task->tensor : task->output;

        if (task->device != CPU_DEVICE_ID) { // GPU
            auto len = task->len;
            auto offset = task->offset;

            BPS_CHECK(tensor->data());
            BPS_CHECK(tensor->data()+offset) << offset;

            auto nccl_dtype = getNcclDataType(tensor->dtype());
            auto unit_len = tensor->size() / tensor->shape().num_elements();

            BPS_LOG(TRACE) << task->tensor_name << " calling NCCL "
                           << LogStrings[this_op]
                           << " (rank=" << rank
                           << ") key=" << key
                           << ", elements=" << len/unit_len
                           << ", device=" << task->device;
            if (this_op == REDUCE) {
                NCCLCHECK(ncclReduce((const void*) (tensor->data()+offset),
                                     (void*) (tensor->data()+offset),
                                     len/unit_len, nccl_dtype, ncclSum, root,
                                     *nccl_comm, *nccl_stream));
            }
            else {
                NCCLCHECK(ncclBroadcast((const void*) (tensor->data()+offset),
                                        (void*) (tensor->data()+offset),
                                        len/unit_len, nccl_dtype, root,
                                        *nccl_comm, *nccl_stream));
            }
        }
    }
    NCCLCHECK(ncclGroupEnd());

    CUDA_CALL(cudaEventCreateWithFlags(&(nccl_entry->cuda_event), cudaEventBlockingSync | cudaEventDisableTiming));
    CUDA_CALL(cudaEventRecord(nccl_entry->cuda_event, *nccl_stream));
    BytePSGlobal::EnqueueNcclGroup(nccl_entry);
    return true;
}

bool RunSyncNcclOnce() {
    auto nccl_entry = BytePSGlobal::DequeueNcclGroup();
    if (nccl_entry) {
        CUDA_CALL(cudaEventSynchronize(nccl_entry->cuda_event));
        for (int i = 0; i < nccl_entry->tasks.size(); i++) {
            FinishOrProceed(nccl_entry->tasks[i]);
            if (nccl_entry->queues.size() > i) {
                nccl_entry->queues[i]->reportFinish(nccl_entry->tasks[i]->len);
            }
        }
        CUDA_CALL(cudaEventDestroy(nccl_entry->cuda_event));
        BPS_LOG(TRACE) << "Finished NCCL Group size=" << nccl_entry->tasks.size()
                       << " rank=" << GetMyLocalRank();
    }
    else {
        std::this_thread::sleep_for(std::chrono::nanoseconds(1000));
    }
    return true;
}

bool RunCopyDevice2HostLoopOnce() {
    QueueType this_op = COPYD2H;
    auto q = BytePSGlobal::GetScheduledQueue(this_op);

    auto copy_d2h_Stream =  BytePSGlobal::GetCopyDevice2HostStream();
    auto task = q->getTask();
    int rank = GetMyLocalRank();

    if (task) {
        BPS_CHECK(IsRoot()) << "only root device should enter COPYD2H loop";
        BPS_CHECK(task->tensor);

        if (task->device != CPU_DEVICE_ID) { // GPU
            auto name = task->tensor_name;
            auto len = task->len;
            auto offset = task->offset;
            auto cpubuff = task->cpubuff + offset;
            BPS_CHECK(cpubuff) << name << ": CPU buffer not initialized, size=" << len;
            CUDA_CALL(cudaMemcpyAsync(cpubuff, task->tensor->data() + offset, len, cudaMemcpyDeviceToHost, *copy_d2h_Stream));
            CUDA_CALL(cudaStreamSynchronize(*copy_d2h_Stream));
        }

        FinishOrProceed(task);
        q->reportFinish(task->len);
    }
    else {
        std::this_thread::sleep_for(std::chrono::nanoseconds(1000));
    }
    return true;
}

bool RunPushLoopOnce() {
    QueueType this_op = PUSH;
    auto q = BytePSGlobal::GetScheduledQueue(this_op);
    auto task = q->getTask();
    if (task) {
        BPS_CHECK(IsRoot()) << "only root device should enter PUSH loop";
        // TODO: allow merging
        auto offset = task->offset;
        auto len = task->len;

        char* data;
        if (task->device != CPU_DEVICE_ID) {
            BPS_CHECK(task->cpubuff + offset);
            data = const_cast<char*> (static_cast<const char*> (task->cpubuff + offset));
        } else {
            BPS_CHECK(task->tensor);
            data = const_cast<char*> (static_cast<const char*> (task->tensor->data() + offset));
        }

        // get metadata
        const int dtype = task->tensor->dtype();

        // false means not to delete data when SArray is deleted
        ps::SArray<char> vals(data, len, false);

        int cmd = GetCommandType(RequestType::kDefaultPushPull, dtype);
        auto& pskv = BytePSGlobal::EncodeDefaultKey(task->key, len);
        BytePSGlobal::GetPS()->ZPush(
            pskv.keys, vals, pskv.lens, cmd,
            [task, q]() {
                FinishOrProceed(task);
                q->reportFinish(task->len);
            }
        );
    }
    else {
        std::this_thread::sleep_for(std::chrono::nanoseconds(1000));
    }
    return true;
}

bool RunPullLoopOnce() {
    QueueType this_op = PULL;
    auto q = BytePSGlobal::GetScheduledQueue(this_op);
    auto task = q->getTask();
    if (task) {
        BPS_CHECK(IsRoot()) << "only root device should enter PULL loop";
        // TODO: allow merging
        auto offset = task->offset;
        auto len = task->len;

        char* data;
        if (task->device != CPU_DEVICE_ID) { // GPU
            BPS_CHECK(task->cpubuff);
            data = const_cast<char*> (static_cast<const char*> (task->cpubuff + offset));
        } else { // CPU
            BPS_CHECK(task->output);
            data = const_cast<char*> (static_cast<const char*> (task->output->data() + offset));
        }

        // get metadata
        const int dtype = task->output->dtype();

        // false means not to delete data when SArray is deleted
        auto vals = new ps::SArray<char>(data, len, false);

        int cmd = GetCommandType(RequestType::kDefaultPushPull, dtype);
        auto& pskv = BytePSGlobal::EncodeDefaultKey(task->key, len);
        // issue pull
        BytePSGlobal::GetPS()->ZPull(
            pskv.keys, vals, &pskv.lens, cmd,
            [vals, task, q]() {
                delete vals;
                FinishOrProceed(task);
                q->reportFinish(task->len);
            });
    }
    else {
        std::this_thread::sleep_for(std::chrono::nanoseconds(1000));
    }
    return true;
}

bool RunCopyHost2DeviceLoopOnce() {
    QueueType this_op = COPYH2D;
    auto q = BytePSGlobal::GetScheduledQueue(this_op);
    auto copy_h2d_Stream = BytePSGlobal::GetCopyHost2DeviceStream();
    auto task = q->getTask();
    int rank = GetMyLocalRank();

    if (task) {
        BPS_CHECK(IsRoot()) << "only root device should enter COPYH2D loop";
        BPS_CHECK(task->output);

        if (task->device != CPU_DEVICE_ID) { // GPU
            auto name = task->tensor_name;
            auto len = task->len;
            auto offset = task->offset;

            auto cpubuff = task->cpubuff + offset;
            BPS_CHECK(cpubuff) << name << ": CPU buffer not initialized, size=" << len;
            char* gpu_addr = const_cast<char*> (static_cast<const char*> (task->output->data() + offset));
            CUDA_CALL(cudaMemcpyAsync(gpu_addr, cpubuff, len, cudaMemcpyHostToDevice, *copy_h2d_Stream));
            CUDA_CALL(cudaStreamSynchronize(*copy_h2d_Stream));
        }

        FinishOrProceed(task);
        q->reportFinish(task->len);
    }
    else {
        std::this_thread::sleep_for(std::chrono::nanoseconds(1000));
    }
    return true;
}

void CoordinateReduceLoop() {
    while (RunCoordinateLoopOnce(COORDINATE_REDUCE) && !BytePSGlobal::ShouldShutdown()) {}
}

void CoordinateBroadcastLoop() {
    while (RunCoordinateLoopOnce(COORDINATE_BROADCAST) && !BytePSGlobal::ShouldShutdown()) {}
}

void RootNcclLoop() {
    CUDA_CALL(cudaSetDevice(BytePSGlobal::GetLocalRank()));
    while (RunRootNcclLoopOnce() && !BytePSGlobal::ShouldShutdown()) {}
}

void NonRootNcclLoop() {
    CUDA_CALL(cudaSetDevice(BytePSGlobal::GetLocalRank()));
    while (RunNonRootNcclLoopOnce() && !BytePSGlobal::ShouldShutdown()) {}
}

void SyncNcclLoop() {
    CUDA_CALL(cudaSetDevice(BytePSGlobal::GetLocalRank()));
    while (RunSyncNcclOnce() && !BytePSGlobal::ShouldShutdown()) {}
}

void CopyDevice2HostLoop() {
    CUDA_CALL(cudaSetDevice(BytePSGlobal::GetLocalRank()));
    while (RunCopyDevice2HostLoopOnce() && !BytePSGlobal::ShouldShutdown()) {}
}

void PushLoop() {
    while (RunPushLoopOnce() && !BytePSGlobal::ShouldShutdown()) {}
}

void PullLoop() {
    while (RunPullLoopOnce() && !BytePSGlobal::ShouldShutdown()) {}
}

void CopyHost2DeviceLoop() {
    CUDA_CALL(cudaSetDevice(BytePSGlobal::GetLocalRank()));
    while (RunCopyHost2DeviceLoopOnce() && !BytePSGlobal::ShouldShutdown()) {}
}

extern "C" {

void byteps_init() {
    BytePSGlobal::Init();
    std::vector<LoopFunction> func;

    if (IsRoot()) {
        func.push_back(RootNcclLoop);
        func.push_back(SyncNcclLoop);
        if (IsDistributedJob()) {
            func.push_back(CopyDevice2HostLoop);
            func.push_back(PushLoop);
            func.push_back(PullLoop);
            func.push_back(CopyHost2DeviceLoop);
        }
    }
    else {
        func.push_back(CoordinateReduceLoop);
        func.push_back(NonRootNcclLoop);
        func.push_back(SyncNcclLoop);
        func.push_back(CoordinateBroadcastLoop);
    }
    
    BytePSGlobal::Start(func);
    return;
}

void byteps_shutdown() {
    BytePSGlobal::Shutdown();
    BPS_LOG(TRACE) << "BytePS is shutdown.";
    return;
}

int byteps_rank() {
    return BytePSGlobal::GetRank();
}

int byteps_local_rank() {
    return BytePSGlobal::GetLocalRank();
}

int byteps_size() {
    return BytePSGlobal::GetSize();
}

int byteps_local_size() {
    return BytePSGlobal::GetLocalSize();
}

} // extern "C"

Status CheckInitialized() {
    return BytePSGlobal::CheckInit();
}

void PartitionTensor(std::shared_ptr<TensorTableEntry> entry,
                    std::vector<std::shared_ptr<TensorTableEntry> > &partitions) {
    BPS_CHECK(entry->counter_ptr) << entry->tensor_name << " counter pointer is null";
    auto size = entry->tensor ? entry->tensor->size() : entry->output->size();
    auto bound = BytePSGlobal::GetPartitionBound();
    auto accumulated = 0;
    int i = 0;

    while (accumulated < size) {
        std::shared_ptr<TensorTableEntry> e(new TensorTableEntry);
        // will assign the key later, so don't do it now
        // e->key = entry->key;
        e->tensor_name = entry->tensor_name + std::string("_") + std::to_string(i);
        e->context = entry->context;
        e->ready_event = entry->ready_event;
        e->device = entry->device;
        e->priority = entry->priority;
        e->version = entry->version;
        e->callback = entry->callback;
        e->cpubuff = entry->cpubuff;
        e->queue_list = entry->queue_list;
        e->tensor = entry->tensor;
        e->output = entry->output;
        e->offset = accumulated;
        e->len = ((size - accumulated) > bound) ? bound : (size - accumulated);
        e->counter_ptr = entry->counter_ptr;
        e->total_partnum = entry->total_partnum;

        accumulated += e->len;
        ++i;

        partitions.push_back(e);
    }
}

Status EnqueueTensor(BPSContext &context,
                     std::shared_ptr<Tensor> input,
                     std::shared_ptr<Tensor> output,
                     std::shared_ptr<ReadyEvent> ready_event,
                     const std::string &name,
                     const int device, const int priority, const int version,
                     StatusCallback callback, std::vector<QueueType> queue_list) {
    if (input && output) {
        BPS_CHECK_EQ(input->size(), output->size()) << name << " output tensor size does not match";
    }

    std::shared_ptr<TensorTableEntry> e(new TensorTableEntry);
    e->tensor_name = name;
    e->context = &context;
    e->tensor = input;
    e->output = output;
    e->ready_event = ready_event;
    e->device = device;
    e->priority = priority;
    e->version = version;
    e->callback = callback;
    e->cpubuff = context.cpubuff;
    e->queue_list = queue_list;
    e->counter_ptr = std::make_shared<std::atomic_int>(0);
    e->total_partnum = context.key_list.size();

    std::vector<std::shared_ptr<TensorTableEntry> > partitions;
    PartitionTensor(e, partitions);
    BPS_CHECK_EQ(context.key_list.size(), partitions.size()) << name
            << ": " << context.key_list.size()
            << ", " << partitions.size();

    if (e->queue_list.size() == 0) {
        BPS_LOG(TRACE) << e->tensor_name << ", device=" << e->device
                       << " has no queue_list assigned, skipped";
        e->callback(Status::OK());
        return Status::OK();
    }

    unsigned int accumulated = 0;
    for (unsigned int i = 0; i < partitions.size(); ++i) {
        auto task = partitions[i];
        task->key = context.key_list[i]; // assign the key now
        BPS_LOG(TRACE) << "EnqueueTensor: " << task->tensor_name
                       << ", key=" << task->key
                       << ", offset=" << task->offset
                       << ", len=" << task->len
                       << ", device=" << task->device
                       << " rank=" << GetMyLocalRank();

        BytePSGlobal::GetScheduledQueue(e->queue_list.at(0))->addTask(task);
        accumulated += task->len;
    }
    BPS_CHECK_EQ(accumulated, (e->tensor ? e->tensor->size(): e->output->size()))
        << "accumulated partition size not equal to original tensor size";

    BPS_LOG(TRACE) << "EnqueueTensor finished: " << name << ", rank=" << GetMyLocalRank();
    return Status::OK();
}

void InitTensor(BPSContext &context, const std::string &name, int dtype, void *cpubuff) {

    size_t size = context.buff_len;

    // Only local root needs to init cpubuff
    if (IsRoot()) {
        if (cpubuff) {
            BPS_LOG(TRACE) << name << " is already on cpu, len=" << size;
            context.cpubuff = cpubuff;
            context.reuse_buff = true;
        }
        else {
            CUDA_CALL(cudaHostAlloc((void **) &(context.cpubuff), size, cudaHostAllocMapped));
            context.reuse_buff = false;
            BPS_LOG(TRACE) << name << ": cudaHostAlloc with len=" << size;
        }
    }

    // Get metadata
    auto key_list = context.key_list;
    char* data = const_cast<char*> (static_cast<const char*> (context.cpubuff));
    auto bound = BytePSGlobal::GetPartitionBound();

    BPS_LOG(TRACE) << "Begin init " << name
                   << ", size=" << size
                   << ", parts=" << key_list.size();

    BPS_CHECK_GT(key_list.size(), 0) << name << " key_list_size=0";
    BPS_CHECK_EQ(key_list.size(), (unsigned int) (size+bound-1)/bound) // round up
                    << key_list.size()
                    << ", size=" << size
                    << ", bound=" << bound;

    unsigned int accumulated = 0;
    unsigned int i = 0;
    while (accumulated < size) {
        auto key = key_list[i];
        int len = ((size - accumulated) > bound) ? bound : (size - accumulated);

        if (IsDistributedJob() && IsRoot() && (BytePSGlobal::GetWorkerID() == 0)) { // only worker0 pushes init data
            auto& pskv = BytePSGlobal::EncodeDefaultKey(key, len);
            // false means not to delete data when SArray is deleted
            ps::SArray<char> vals(data + accumulated, len, false);
            int cmd = GetCommandType(RequestType::kDefaultPushPull, dtype);
            BytePSGlobal::GetPS()->Wait(BytePSGlobal::GetPS()->ZPush(
                pskv.keys, vals, pskv.lens, cmd));
        }

        if (IsDistributedJob() && IsRoot()) { // all worker need to sync
            ps::Postoffice::Get()->Barrier(0, ps::kWorkerGroup);
        }

        accumulated += len;
        ++i;
    }

    BPS_CHECK_EQ(accumulated, size);
    BPS_CHECK_EQ(i, key_list.size());

    context.initialized = true;

    BPS_LOG(TRACE) << "Finish Init " << name
                   << ", size=" << size
                   << ", parts=" << key_list.size();
}

Status EnqueueTensorInit(BPSContext &context, const std::string &name, int dtype, void *cpubuff,
                         StatusCallback callback) {
    InitTensor(context, name, dtype, cpubuff);
    callback(Status::OK());
    return Status::OK();
}

BPSContext& GetContextFromName(const std::string &name) {
    return BytePSGlobal::GetContextFromName(name);
}

bool IsTensorInitialized(const std::string &name, size_t size) {
    return BytePSGlobal::IsTensorInitialized(name, size);
}

bool IsRoot() {
    return BytePSGlobal::IsRootDevice();
}

int GetRoot() {
    return BytePSGlobal::GetRoot();
}

int GetMyLocalRank() {
    return BytePSGlobal::GetLocalRank();
}

bool IsDistributedJob() {
    return BytePSGlobal::IsDistributed();
}

} // namespace common
} // namespace byteps
