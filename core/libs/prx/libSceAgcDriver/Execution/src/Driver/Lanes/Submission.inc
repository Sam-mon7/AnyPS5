void Driver::enqueue(Submission submission) {
    const auto queue = submission.queue;
    submission.waitFree = waitFree(submission);
    auto& worker = workers[queue];
    for (const auto dword : submission.labelWrites) ++worker.unfinishedWrites[dword];
    if (!Lanes::Enabled()) {
        worker.pending.push_back(std::move(submission));
        worker.queued.fetch_add(1, std::memory_order_acq_rel);
        if (!worker.thread.joinable()) worker.thread = std::thread([this, queue] {
            char role[32];
            std::snprintf(role, sizeof(role), "queue 0x%x worker", queue);
            try {
                RaiseWorkerThreadPriority(role);
            } catch (...) {
                ReportFailure(std::current_exception());
                return;
            }
            run(queue);
        });
        return;
    }
    static auto* scheduler = new Lanes::Scheduler();
    worker.queued.fetch_add(1, std::memory_order_acq_rel);
    auto current = std::make_shared<Lanes::SubmissionCursor>();
    current->writes = submission.labelWrites;
    current->submission = std::move(submission);
    // ponytail: legacy recording stays serial until G4-G7 provide concurrent resource and recorder state.
    auto executeChunk = [this](const Submission& chunk) {
        const Lanes::QueueScope queueScope(chunk.queue);
        onWorkerThread() = true;
        workerQueued() = nullptr;
        DrawPipeline::Active() = false;
        GuestMemory::TagGpuLockThread(chunk.queue);
        struct Running {
            Driver& driver;
            explicit Running(Driver& driver) : driver(driver) { driver.runningWorkers.fetch_add(1, std::memory_order_acq_rel); }
            ~Running() {
                if (driver.runningWorkers.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                    driver.idleEpoch.fetch_add(1, std::memory_order_release);
                    driver.changed.notify_all();
                }
            }
        } running{*this};
        CheckFailure();
        bumpEpoch(&EpochBumps::submissions);
        execute(chunk);
        if (Graphics::Recorder::ComputeChainsEnabled()) {
            if (chunk.queue == 0 && DrawPipeline::Depth() != 0) DrawPipeline::Queue0().Drain(DrawPipeline::DrainReason::Submission);
            std::lock_guard gpuLock(GuestMemory::GpuMutex());
            if (auto* recorder = Graphics::Recorder::Active()) recorder->EndComputeChain(Graphics::Recorder::ComputeChainReason::Lanes);
        }
    };
    scheduler->Enqueue(queue, {
        [this, current, executeChunk]() -> Lanes::Step {
            auto& source = current->submission;
            if (!current->started) {
                std::lock_guard lock(mutex);
                checkStopping();
                workers.at(source.queue).queued.fetch_sub(1, std::memory_order_acq_rel);
                source.dequeuedAt = source.orderedAt = std::chrono::steady_clock::now();
                if (source.queue == 0) queue0Executing = source.received;
                current->started = true;
            }
            if (source.suspend) {
                source.suspend = false;
                Submission boundary = source;
                boundary.suspend = true;
                return {Lanes::Chunk{source.queue, 0, [boundary = std::move(boundary), executeChunk](std::size_t) {
                    return [boundary, executeChunk] { executeChunk(boundary); };
                }}, false};
            }
            if (current->cursor == source.commands.size()) {
                if (source.rewindTail == nullptr) return {{}, true};
                auto* control = const_cast<std::uint32_t*>(source.rewindTail - 1);
                if ((std::atomic_ref<std::uint32_t>(*control).load(std::memory_order_acquire) & 0x80000000u) == 0) {
                    if (source.queue == 0) {
                        queue0Awaited.store(reinterpret_cast<std::uint64_t>(control), std::memory_order_release);
                        noteAwaitingTitle(0, reinterpret_cast<std::uint64_t>(control));
                    }
                    return {};
                }
                return {Lanes::Chunk{source.queue, 0, [this, current](std::size_t) -> Lanes::Commit {
                    return [this, current] {
                        onWorkerThread() = true;
                        CheckFailure();
                        auto& old = current->submission;
                        Submission tail{};
                        tail.queue = old.queue;
                        tail.serial = old.serial;
                        tail.receivedAt = std::chrono::steady_clock::now();
                        copyCommands(tail, old.rewindTail, old.rewindWords);
                        tail.copiedAt = std::chrono::steady_clock::now();
                        validate(tail, old.rewindTail);
                        readRegisterLists(tail);
                        tail.validatedAt = std::chrono::steady_clock::now();
                        waitForFlipRoom(tail);
                        tail.roomReadyAt = std::chrono::steady_clock::now();
                        {
                            std::lock_guard lock(mutex);
                            checkStopping();
                            reserveOutputs(tail);
                            tail.shaders = shaders;
                            tail.received = ++eventSerial;
                            for (const auto address : tail.labelWrites) ++workers.at(tail.queue).unfinishedWrites[address];
                        }
                        current->writes.insert(current->writes.end(), tail.labelWrites.begin(), tail.labelWrites.end());
                        tail.enqueuedAt = tail.dequeuedAt = tail.orderedAt = std::chrono::steady_clock::now();
                        old = std::move(tail);
                        current->cursor = 0;
                        current->predicateChecked.reset();
                        if (old.queue == 0) {
                            queue0Awaited.store(0, std::memory_order_release);
                            queue0AwaitsTitle.store(false, std::memory_order_release);
                        }
                    };
                }}, false};
            }
            const auto packet = current->Packet();
            const auto opcode = (packet[0] >> 8u) & 0xffu;
            const bool wait = !Pm4::FillerPacket(packet[0]) && (opcode == 0x3c || opcode == 0x93);
            if (wait && (!Pm4::Predicated(packet[0]) || current->predicateChecked == current->cursor)) {
                const auto address = packet[2] | (static_cast<std::uint64_t>(packet[3]) << 32u);
                const auto bytes = Pm4::WaitAwaitedBytes(packet);
                GuestMemory::CheckRange(reinterpret_cast<const void*>(address), bytes, bytes);
                if (!source.heldAtSubmit.contains(current->cursor) && !Pm4::WaitSatisfiedUnchecked(packet)) {
                    if (source.queue == 0) {
                        queue0Awaited.store(address, std::memory_order_release);
                        noteAwaitingTitle(0, address);
                    }
                    return {};
                }
                current->cursor += packet.size();
                current->predicateChecked.reset();
                if (source.queue == 0) {
                    queue0Awaited.store(0, std::memory_order_release);
                    queue0AwaitsTitle.store(false, std::memory_order_release);
                }
                return {Lanes::Chunk{source.queue, 0, [this, queue = source.queue, address, bytes](std::size_t) -> Lanes::Commit {
                    return [this, queue, address, bytes] {
                        if (Lanes::AsyncComputeEnabled()) if (const auto localDevice = device.Load()) {
                            if (const auto queues = Lanes::FindQueues(localDevice->Device()); queues && queues->Dedicated() && queues->Timeline()) queues->WaitLabel(queue, address, bytes);
                        }
                        bumpEpoch(&EpochBumps::waits);
                        gateOpened() = true;
                    };
                }}, false};
            }
            if (!Pm4::FillerPacket(packet[0]) && (opcode == 0x22 || opcode == 0x3f || wait)) {
                return {Lanes::Chunk{source.queue, 0, [this, current](std::size_t) -> Lanes::Commit {
                    return [this, current] {
                        onWorkerThread() = true;
                        const auto packet = current->Packet();
                        const auto opcode = (packet[0] >> 8u) & 0xffu;
                        const auto queueId = current->submission.queue;
                        GuestMemory::TagGpuLockThread(queueId);
                        CheckFailure();
                        bumpEpoch(&EpochBumps::submissions);
                        recordQueuedLabelsBeforeRead(queueId);
                        QueueState* state = nullptr;
                        {
                            std::lock_guard lock(mutex);
                            if (queueId == 0 && resetGraphics) {
                                queues.erase(0);
                                resetGraphics = false;
                            }
                            state = &queues[queueId];
                        }
                        const bool skipped = Pm4::Predicated(packet[0]) && state->predication.operation != 0 && !Pm4::PredicationPasses(*state);
                        if (skipped) current->cursor = opcode == 0x3f ? current->submission.conditionalEnds.at(current->cursor) : current->cursor + packet.size();
                        else if (opcode == 0x22) current->cursor = Pm4::ReadCondition(packet) == 0 ? current->submission.conditionalEnds.at(current->cursor) : current->cursor + packet.size();
                        else if (opcode == 0x3f) current->cursor += packet.size();
                        else current->predicateChecked = current->cursor;
                    };
                }}, false};
            }
            const auto end = current->End();
            return {Lanes::Chunk{source.queue, 0, [current, end, executeChunk](std::size_t) -> Lanes::Commit {
                auto chunk = current->Slice(end);
                return [current, end, chunk = std::move(chunk), executeChunk] {
                    if (chunk.holdsFlip) current->submission.holdsFlip = false;
                    executeChunk(chunk);
                    current->cursor = end;
                    current->predicateChecked.reset();
                };
            }}, false};
        },
        [this, current](std::exception_ptr error) {
            auto& source = current->submission;
            if (error != nullptr) {
                for (const auto& [offset, flip] : source.flips) flip->Fail(error);
                if (source.holdsFlip) releaseFlipHold();
                if (!stopping.load(std::memory_order_acquire) && !failed.load(std::memory_order_acquire)) ReportFailure(error);
                return;
            }
            {
                std::lock_guard lock(mutex);
                markCompleted(source.serial);
                forgetUnfinishedWrites(workers.at(source.queue), current->writes);
                if (source.queue == 0) queue0Executing = 0;
            }
            changed.notify_all();
        }
    });
    auto& service = workers[0];
    if (!service.thread.joinable()) service.thread = std::thread([this] {
        onWorkerThread() = true;
        try {
            RaiseWorkerThreadPriority("GPU lanes parser");
            auto quietSince = std::chrono::steady_clock::now();
            scheduler->Run([this] { return stopping.load(std::memory_order_acquire) || failed.load(std::memory_order_acquire) || shutdownToken.stop_requested(); }, [this, &quietSince] {
                reapCompletionLabels();
                const auto now = std::chrono::steady_clock::now();
                if (runningWorkers.load(std::memory_order_acquire) != 0 || completionsPending()) quietSince = now;
                else if (now - quietSince >= std::chrono::milliseconds(50)) {
                    if (Graphics::StorageTexture::AnyPending()) if (const auto localDevice = device.Load()) localDevice->StoreIdleResults(false);
                    quietSince = now;
                }
            });
        } catch (...) {
            const auto error = std::current_exception();
            ReportFailure(error);
            scheduler->Cancel(error);
        }
    });
}
