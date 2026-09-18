#include <fuse/assert.hpp>
#include <fuse/log/logger.hpp>
#include <fuse/platform/thread.hpp>
#include <fuse/profiler/profiler.hpp>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

namespace {

int g_failures = 0;

void expectTrue(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

void resetState() {
    fuse::profiler::reset();
    fuse::profiler::setEnabled(true);
    fuse::assertion::clearFatalHandler();
    fuse::assertion::setSuppressAbortForTests(false);
}

void testProfileScopeRecordsEvents() {
    resetState();

    {
        FUSE_PROFILE_SCOPE("test_scope");
    }

    expectTrue(fuse::profiler::eventCount() >= 2u, "scope records begin and end events");
    const fuse::profiler::ProfileEvent& begin = fuse::profiler::eventAt(0);
    const fuse::profiler::ProfileEvent& end = fuse::profiler::eventAt(1);
    expectTrue(begin.phase == fuse::profiler::EventPhase::Begin, "first event is begin");
    expectTrue(end.phase == fuse::profiler::EventPhase::End, "second event is end");
    expectTrue(begin.name != nullptr && std::string(begin.name) == "test_scope", "scope name preserved");
    expectTrue(end.timestampNs >= begin.timestampNs, "end timestamp is not before begin");
}

void testProfilerDisabledSkipsEvents() {
    resetState();
    fuse::profiler::setEnabled(false);

    {
        FUSE_PROFILE_SCOPE("ignored");
    }

    expectTrue(fuse::profiler::eventCount() == 0u, "disabled profiler records nothing");
}

void testFrameBoundaryIncrementsIndex() {
    resetState();

    const fuse::u32 before = fuse::profiler::frameIndex();
    fuse::profiler::beginFrame();
    fuse::profiler::endFrame();
    expectTrue(fuse::profiler::frameIndex() == before + 1u, "beginFrame increments frame index");
}

void testNestedScopeOrdering() {
    resetState();

    {
        FUSE_PROFILE_SCOPE("outer");
        {
            FUSE_PROFILE_SCOPE("inner");
            {
                FUSE_PROFILE_SCOPE("deepest");
            }
        }
    }

    expectTrue(fuse::profiler::eventCount() == 6u, "three nested scopes emit six events");
    expectTrue(fuse::profiler::maxNestingDepth() == 3u, "max nesting depth tracks deepest scope");

    const fuse::profiler::ProfileEvent& e0 = fuse::profiler::eventAt(0);
    const fuse::profiler::ProfileEvent& e1 = fuse::profiler::eventAt(1);
    const fuse::profiler::ProfileEvent& e2 = fuse::profiler::eventAt(2);
    const fuse::profiler::ProfileEvent& e3 = fuse::profiler::eventAt(3);
    const fuse::profiler::ProfileEvent& e4 = fuse::profiler::eventAt(4);
    const fuse::profiler::ProfileEvent& e5 = fuse::profiler::eventAt(5);

    expectTrue(e0.phase == fuse::profiler::EventPhase::Begin && std::string(e0.name) == "outer",
               "nested trace begins with outer");
    expectTrue(e1.phase == fuse::profiler::EventPhase::Begin && std::string(e1.name) == "inner",
               "nested trace continues with inner begin");
    expectTrue(e2.phase == fuse::profiler::EventPhase::Begin && std::string(e2.name) == "deepest",
               "nested trace reaches deepest begin");
    expectTrue(e3.phase == fuse::profiler::EventPhase::End && std::string(e3.name) == "deepest",
               "deepest scope ends before inner");
    expectTrue(e4.phase == fuse::profiler::EventPhase::End && std::string(e4.name) == "inner",
               "inner scope ends before outer");
    expectTrue(e5.phase == fuse::profiler::EventPhase::End && std::string(e5.name) == "outer",
               "outer scope ends last");

    expectTrue(e0.nestingDepth == 1u && e1.nestingDepth == 2u && e2.nestingDepth == 3u,
               "nesting depth increments per scope level");
    expectTrue(e0.scopeId == e5.scopeId, "outer begin/end share scope id");
    expectTrue(e2.scopeId == e3.scopeId, "deepest begin/end share scope id");
}

void testThreadIdStubs() {
    resetState();
    fuse::platform::registerMainThread();

    {
        FUSE_PROFILE_SCOPE("main_thread_scope");
    }

    const fuse::profiler::ProfileEvent& begin = fuse::profiler::eventAt(0);
    expectTrue(begin.threadId != 0u, "profiler records non-zero chrome tid");
    expectTrue(begin.threadId == fuse::platform::chromeTraceThreadId(),
               "profiler tid matches platform chromeTraceThreadId");
    expectTrue(fuse::platform::isMainThread(), "main thread stub reports current thread as main");

    std::atomic<bool> workerDone{false};
    std::atomic<fuse::u32> workerThreadTid{0};
    std::thread worker([&]() {
        workerThreadTid.store(fuse::platform::chromeTraceThreadId(), std::memory_order_release);
        FUSE_PROFILE_SCOPE("worker_scope");
        workerDone.store(true, std::memory_order_release);
    });
    worker.join();
    expectTrue(workerDone.load(std::memory_order_acquire), "worker thread completed");

    expectTrue(workerThreadTid.load(std::memory_order_acquire) != begin.threadId,
               "worker thread tid differs from main thread tid");
    expectTrue(fuse::profiler::eventCount() >= 4u, "worker scope adds begin/end events");

    const fuse::profiler::ProfileEvent& workerBegin = fuse::profiler::eventAt(2);
    expectTrue(workerBegin.threadId == workerThreadTid.load(std::memory_order_acquire),
               "worker event tid matches worker chromeTraceThreadId");
    expectTrue(std::string(workerBegin.name) == "worker_scope", "worker scope name preserved");
}

void testChromeTraceExport() {
    resetState();
    fuse::platform::registerMainThread();

    {
        FUSE_PROFILE_SCOPE("chrome_scope");
        {
            FUSE_PROFILE_SCOPE("chrome_child");
        }
    }

    const std::string json = fuse::profiler::exportChromeTraceJson();
    expectTrue(json.find("\"displayTimeUnit\":\"ns\"") != std::string::npos,
               "chrome trace declares displayTimeUnit");
    expectTrue(json.find("\"metadata\":{\"name\":\"FUSE CPU profiler\"") != std::string::npos,
               "chrome trace includes metadata block");
    expectTrue(json.find("\"frame\":") != std::string::npos, "chrome trace exports frame metadata");
    expectTrue(json.find("\"traceEvents\":[") != std::string::npos, "chrome trace has traceEvents root");
    expectTrue(json.find("\"name\":\"chrome_scope\"") != std::string::npos, "chrome trace includes scope name");
    expectTrue(json.find("\"name\":\"chrome_child\"") != std::string::npos, "chrome trace includes nested scope");
    expectTrue(json.find("\"ph\":\"B\"") != std::string::npos, "chrome trace has begin phase");
    expectTrue(json.find("\"ph\":\"E\"") != std::string::npos, "chrome trace has end phase");
    expectTrue(json.find("\"pid\":1") != std::string::npos, "chrome trace pins process id");
    expectTrue(json.find("\"tid\":") != std::string::npos, "chrome trace exports thread id");
    expectTrue(json.find("\"id\":") != std::string::npos, "chrome trace pairs begin/end with id");
    expectTrue(json.find("\"args\":{\"depth\":") != std::string::npos, "chrome trace exports nesting depth args");
    expectTrue(json.back() == '}', "chrome trace json is closed");
}

void testAsyncFlowStubs() {
    resetState();
    fuse::platform::registerMainThread();

    const fuse::u32 flowId = fuse::profiler::nextFlowId();
    expectTrue(flowId != 0u, "nextFlowId returns non-zero id");

    FUSE_PROFILE_ASYNC_FLOW_BEGIN("vfs_load", flowId);
    FUSE_PROFILE_ASYNC_FLOW_END("vfs_load", flowId);

    expectTrue(fuse::profiler::eventCount() == 2u, "async flow emits start and finish events");

    const fuse::profiler::ProfileEvent& flowStart = fuse::profiler::eventAt(0);
    const fuse::profiler::ProfileEvent& flowFinish = fuse::profiler::eventAt(1);
    expectTrue(flowStart.phase == fuse::profiler::EventPhase::FlowStart, "first flow event is start");
    expectTrue(flowFinish.phase == fuse::profiler::EventPhase::FlowFinish, "second flow event is finish");
    expectTrue(std::string(flowStart.name) == "vfs_load", "flow start preserves name");
    expectTrue(std::string(flowFinish.name) == "vfs_load", "flow finish preserves name");
    expectTrue(flowStart.scopeId == flowId, "flow start stores flow id");
    expectTrue(flowFinish.scopeId == flowId, "flow finish stores flow id");
    expectTrue(flowFinish.timestampNs >= flowStart.timestampNs,
               "flow finish timestamp is not before flow start");

    const std::string json = fuse::profiler::exportChromeTraceJson();
    expectTrue(json.find("\"cat\":\"async\"") != std::string::npos, "async flow uses async category");
    expectTrue(json.find("\"ph\":\"s\"") != std::string::npos, "chrome trace exports flow start phase");
    expectTrue(json.find("\"ph\":\"f\"") != std::string::npos, "chrome trace exports flow finish phase");
    expectTrue(json.find("\"name\":\"vfs_load\"") != std::string::npos, "chrome trace includes flow name");
    expectTrue(json.find("\"bp\":\"e\"") != std::string::npos, "flow finish binds to enclosing slice end");
}

void testAsyncFlowCrossThread() {
    resetState();
    fuse::platform::registerMainThread();

    const fuse::u32 flowId = 42u;
    FUSE_PROFILE_ASYNC_FLOW_BEGIN("job_handoff", flowId);

    std::atomic<bool> workerDone{false};
    std::atomic<fuse::u32> workerTid{0};
    std::thread worker([&]() {
        workerTid.store(fuse::platform::chromeTraceThreadId(), std::memory_order_release);
        FUSE_PROFILE_ASYNC_FLOW_END("job_handoff", flowId);
        workerDone.store(true, std::memory_order_release);
    });
    worker.join();
    expectTrue(workerDone.load(std::memory_order_acquire), "worker thread completed");

    expectTrue(fuse::profiler::eventCount() == 2u, "cross-thread flow emits two events");
    const fuse::profiler::ProfileEvent& flowStart = fuse::profiler::eventAt(0);
    const fuse::profiler::ProfileEvent& flowFinish = fuse::profiler::eventAt(1);
    expectTrue(flowStart.threadId != flowFinish.threadId, "flow start and finish record distinct tids");
    expectTrue(flowFinish.threadId == workerTid.load(std::memory_order_acquire),
               "flow finish tid matches worker chromeTraceThreadId");
}

void testCounterSamples() {
    resetState();
    fuse::platform::registerMainThread();

    FUSE_PROFILE_COUNTER("frame_alloc_bytes", 4096);
    FUSE_PROFILE_COUNTER("active_jobs", 3);

    expectTrue(fuse::profiler::eventCount() == 2u, "counter samples emit one event each");

    const fuse::profiler::ProfileEvent& first = fuse::profiler::eventAt(0);
    const fuse::profiler::ProfileEvent& second = fuse::profiler::eventAt(1);
    expectTrue(first.phase == fuse::profiler::EventPhase::Counter, "counter event phase is Counter");
    expectTrue(second.phase == fuse::profiler::EventPhase::Counter, "second counter event phase is Counter");
    expectTrue(std::string(first.name) == "frame_alloc_bytes", "counter track name preserved");
    expectTrue(first.counterKind == fuse::profiler::CounterValueKind::Int, "int counter kind preserved");
    expectTrue(first.counterIntValue == 4096, "counter value preserved");
    expectTrue(second.counterIntValue == 3, "second counter value preserved");

    const std::string json = fuse::profiler::exportChromeTraceJson();
    expectTrue(json.find("\"cat\":\"counter\"") != std::string::npos, "counter uses counter category");
    expectTrue(json.find("\"ph\":\"C\"") != std::string::npos, "chrome trace exports counter phase");
    expectTrue(json.find("\"name\":\"frame_alloc_bytes\"") != std::string::npos,
               "chrome trace includes counter track name");
    expectTrue(json.find("\"args\":{\"value\":4096}") != std::string::npos,
               "chrome trace exports counter value args");
    expectTrue(json.find("\"args\":{\"value\":3}") != std::string::npos,
               "chrome trace exports second counter value");
}

void testFloatCounterSamples() {
    resetState();
    fuse::platform::registerMainThread();

    FUSE_PROFILE_COUNTER("frame_time_ms", 16.667);
    FUSE_PROFILE_COUNTER("gpu_utilization", 0.75);

    expectTrue(fuse::profiler::eventCount() == 2u, "float counter samples emit one event each");

    const fuse::profiler::ProfileEvent& first = fuse::profiler::eventAt(0);
    const fuse::profiler::ProfileEvent& second = fuse::profiler::eventAt(1);
    expectTrue(first.phase == fuse::profiler::EventPhase::Counter, "float counter event phase is Counter");
    expectTrue(first.counterKind == fuse::profiler::CounterValueKind::Float, "float counter kind preserved");
    expectTrue(first.counterFloatValue > 16.666 && first.counterFloatValue < 16.668,
               "float counter value preserved");
    expectTrue(second.counterFloatValue == 0.75, "second float counter value preserved");

    const std::string json = fuse::profiler::exportChromeTraceJson();
    expectTrue(json.find("\"name\":\"frame_time_ms\"") != std::string::npos,
               "chrome trace includes float counter track name");
    expectTrue(json.find("\"args\":{\"value\":16.667") != std::string::npos,
               "chrome trace exports float counter value args");
    expectTrue(json.find("\"args\":{\"value\":0.75}") != std::string::npos,
               "chrome trace exports second float counter value");
}

void testAsyncFlowMatchingIdsInExport() {
    resetState();
    fuse::platform::registerMainThread();

    const fuse::u32 flowId = 77u;
    FUSE_PROFILE_ASYNC_FLOW_BEGIN("paired_flow", flowId);
    FUSE_PROFILE_ASYNC_FLOW_END("paired_flow", flowId);

    const std::string json = fuse::profiler::exportChromeTraceJson();
    const std::string flowIdToken = "\"id\":" + std::to_string(flowId);
    const auto firstId = json.find(flowIdToken);
    const auto secondId = json.find(flowIdToken, firstId == std::string::npos ? 0u : firstId + 1u);
    expectTrue(firstId != std::string::npos, "flow start exports matching flow id");
    expectTrue(secondId != std::string::npos, "flow finish exports matching flow id");
    expectTrue(json.find("\"ph\":\"s\"") != std::string::npos && json.find("\"ph\":\"f\"") != std::string::npos,
               "paired flow exports start and finish phases");
}

void testChromeTraceNestingDepthExport() {
    resetState();
    fuse::platform::registerMainThread();

    {
        FUSE_PROFILE_SCOPE("depth_outer");
        {
            FUSE_PROFILE_SCOPE("depth_inner");
        }
    }

    const std::string json = fuse::profiler::exportChromeTraceJson();
    expectTrue(json.find("\"args\":{\"depth\":1}") != std::string::npos,
               "chrome trace exports outer scope depth");
    expectTrue(json.find("\"args\":{\"depth\":2}") != std::string::npos,
               "chrome trace exports inner scope depth");
    expectTrue(fuse::profiler::maxNestingDepth() == 2u, "nesting depth tracks two-level stack");
}

void testChromeTraceEmptyExport() {
    resetState();
    fuse::platform::registerMainThread();

    expectTrue(fuse::profiler::eventCount() == 0u, "reset leaves profiler buffer empty");
    const std::string json = fuse::profiler::exportChromeTraceJson();
    expectTrue(json.find("\"displayTimeUnit\":\"ns\"") != std::string::npos,
               "empty export declares displayTimeUnit");
    expectTrue(json.find("\"metadata\":{\"name\":\"FUSE CPU profiler\",\"frame\":0}") != std::string::npos,
               "empty export includes frame metadata");
    expectTrue(json.find("\"traceEvents\":[]") != std::string::npos,
               "empty export emits zero trace events");
    expectTrue(json.back() == '}', "empty export json is closed");
}

void testNestedAsyncFlowWithinScope() {
    resetState();
    fuse::platform::registerMainThread();

    const fuse::u32 flowId = fuse::profiler::nextFlowId();
    {
        FUSE_PROFILE_SCOPE("flow_outer");
        {
            FUSE_PROFILE_SCOPE("flow_inner");
            FUSE_PROFILE_ASYNC_FLOW_BEGIN("nested_io", flowId);
            FUSE_PROFILE_COUNTER("nested_budget", 12);
            FUSE_PROFILE_ASYNC_FLOW_END("nested_io", flowId);
        }
    }

    expectTrue(fuse::profiler::eventCount() == 7u,
               "nested flow inside scopes emits scope + flow + counter events");

    const fuse::profiler::ProfileEvent& flowStart = fuse::profiler::eventAt(2);
    const fuse::profiler::ProfileEvent& counter = fuse::profiler::eventAt(3);
    const fuse::profiler::ProfileEvent& flowFinish = fuse::profiler::eventAt(4);
    expectTrue(flowStart.phase == fuse::profiler::EventPhase::FlowStart, "nested flow start recorded");
    expectTrue(flowFinish.phase == fuse::profiler::EventPhase::FlowFinish, "nested flow finish recorded");
    expectTrue(flowStart.nestingDepth == 2u, "flow start inherits inner scope depth");
    expectTrue(flowFinish.nestingDepth == 2u, "flow finish inherits inner scope depth");
    expectTrue(counter.nestingDepth == 2u, "counter sample inherits inner scope depth");
    expectTrue(counter.flowNestingDepth == 1u, "counter sample inherits async flow depth");

    const std::string json = fuse::profiler::exportChromeTraceJson();
    expectTrue(json.find("\"name\":\"nested_io\"") != std::string::npos,
               "nested flow export keeps flow name");
    expectTrue(json.find("\"args\":{\"depth\":2}") != std::string::npos,
               "nested flow export includes inner scope depth");
    expectTrue(json.find("\"args\":{\"value\":12,\"depth\":2,\"flow_depth\":1}") != std::string::npos,
               "nested counter export includes scope and flow depth");
}

void testDisabledProfilerSkipsAsyncFlowAndCounter() {
    resetState();
    fuse::profiler::setEnabled(false);

    const fuse::u32 flowId = fuse::profiler::nextFlowId();
    FUSE_PROFILE_ASYNC_FLOW_BEGIN("ignored_flow", flowId);
    FUSE_PROFILE_COUNTER("ignored_counter", 99);
    FUSE_PROFILE_ASYNC_FLOW_END("ignored_flow", flowId);

    expectTrue(fuse::profiler::eventCount() == 0u,
               "disabled profiler skips async flow and counter samples");
    expectTrue(fuse::profiler::exportChromeTraceJson().find("\"traceEvents\":[]") != std::string::npos,
               "disabled profiler export stays empty");
}

void testChromeTraceEscapedNameExport() {
    resetState();
    fuse::platform::registerMainThread();

    FUSE_PROFILE_COUNTER("track\"quoted\\path", 1);

    const std::string json = fuse::profiler::exportChromeTraceJson();
    expectTrue(json.find("\"name\":\"track\\\"quoted\\\\path\"") != std::string::npos,
               "chrome export escapes quotes and backslashes in counter track names");
}

void testChromeTraceEscapedControlCharsExport() {
    resetState();
    fuse::platform::registerMainThread();

    FUSE_PROFILE_COUNTER("line\nbreak\ttab", 2);

    const std::string json = fuse::profiler::exportChromeTraceJson();
    expectTrue(json.find("\"name\":\"line\\nbreak\\ttab\"") != std::string::npos,
               "chrome export escapes control characters in counter track names");
}

void testChromeTraceEscapedScopeAndFlowNames() {
    resetState();
    fuse::platform::registerMainThread();

    const fuse::u32 flowId = fuse::profiler::nextFlowId();
    {
        FUSE_PROFILE_SCOPE("scope\"name\\v1");
        FUSE_PROFILE_ASYNC_FLOW_BEGIN("flow\nstart", flowId);
        FUSE_PROFILE_ASYNC_FLOW_END("flow\nstart", flowId);
    }

    const std::string json = fuse::profiler::exportChromeTraceJson();
    expectTrue(json.find("\"name\":\"scope\\\"name\\\\v1\"") != std::string::npos,
               "chrome export escapes quotes and backslashes in scope names");
    expectTrue(json.find("\"name\":\"flow\\nstart\"") != std::string::npos,
               "chrome export escapes newlines in async flow names");
}

void testCounterSampleInsideScopeDepthExport() {
    resetState();
    fuse::platform::registerMainThread();

    {
        FUSE_PROFILE_SCOPE("budget_parent");
        FUSE_PROFILE_COUNTER("scoped_budget", 42);
    }

    const fuse::profiler::ProfileEvent& counter = fuse::profiler::eventAt(1);
    expectTrue(counter.phase == fuse::profiler::EventPhase::Counter, "counter event recorded inside scope");
    expectTrue(counter.nestingDepth == 1u, "counter inside one scope inherits depth 1");

    const std::string json = fuse::profiler::exportChromeTraceJson();
    expectTrue(json.find("\"args\":{\"value\":42,\"depth\":1}") != std::string::npos,
               "chrome export includes depth for counter sampled inside scope");
}

void testMultipleAsyncFlowsInNestedScopes() {
    resetState();
    fuse::platform::registerMainThread();

    const fuse::u32 outerFlowId = fuse::profiler::nextFlowId();
    const fuse::u32 innerFlowId = fuse::profiler::nextFlowId();
    expectTrue(innerFlowId > outerFlowId, "nextFlowId is monotonic");

    {
        FUSE_PROFILE_SCOPE("multi_outer");
        FUSE_PROFILE_ASYNC_FLOW_BEGIN("outer_io", outerFlowId);
        {
            FUSE_PROFILE_SCOPE("multi_inner");
            FUSE_PROFILE_ASYNC_FLOW_BEGIN("inner_io", innerFlowId);
            FUSE_PROFILE_ASYNC_FLOW_END("inner_io", innerFlowId);
        }
        FUSE_PROFILE_ASYNC_FLOW_END("outer_io", outerFlowId);
    }

    expectTrue(fuse::profiler::eventCount() == 8u,
               "nested concurrent flows emit scope + paired flow events");

    const fuse::profiler::ProfileEvent& outerStart = fuse::profiler::eventAt(1);
    const fuse::profiler::ProfileEvent& innerStart = fuse::profiler::eventAt(3);
    const fuse::profiler::ProfileEvent& innerFinish = fuse::profiler::eventAt(4);
    const fuse::profiler::ProfileEvent& outerFinish = fuse::profiler::eventAt(6);
    expectTrue(outerStart.phase == fuse::profiler::EventPhase::FlowStart, "outer flow start recorded");
    expectTrue(innerStart.phase == fuse::profiler::EventPhase::FlowStart, "inner flow start recorded");
    expectTrue(innerFinish.phase == fuse::profiler::EventPhase::FlowFinish, "inner flow finishes before outer");
    expectTrue(outerFinish.phase == fuse::profiler::EventPhase::FlowFinish, "outer flow finish follows inner scope end");
    expectTrue(outerStart.nestingDepth == 1u, "outer flow inherits outer scope depth");
    expectTrue(innerStart.nestingDepth == 2u, "inner flow inherits inner scope depth");
    expectTrue(outerStart.scopeId == outerFlowId, "outer flow id preserved");
    expectTrue(innerStart.scopeId == innerFlowId, "inner flow id preserved");

    const std::string json = fuse::profiler::exportChromeTraceJson();
    const std::string outerIdToken = "\"id\":" + std::to_string(outerFlowId);
    const std::string innerIdToken = "\"id\":" + std::to_string(innerFlowId);
    expectTrue(json.find(outerIdToken) != std::string::npos, "outer flow id exported");
    expectTrue(json.find(innerIdToken) != std::string::npos, "inner flow id exported");
    expectTrue(json.find("\"name\":\"outer_io\"") != std::string::npos, "outer flow name exported");
    expectTrue(json.find("\"name\":\"inner_io\"") != std::string::npos, "inner flow name exported");
}

void testChromeTraceExportFrameIndex() {
    resetState();
    fuse::platform::registerMainThread();

    fuse::profiler::beginFrame();
    fuse::profiler::beginFrame();
    fuse::profiler::endFrame();

    const std::string json = fuse::profiler::exportChromeTraceJson();
    expectTrue(json.find("\"metadata\":{\"name\":\"FUSE CPU profiler\",\"frame\":2}") != std::string::npos,
               "chrome export frame metadata reflects beginFrame count");
}

void testChromeTraceExportMixedEvents() {
    resetState();
    fuse::platform::registerMainThread();

    const fuse::u32 flowId = fuse::profiler::nextFlowId();
    {
        FUSE_PROFILE_SCOPE("mixed_parent");
        FUSE_PROFILE_ASYNC_FLOW_BEGIN("mixed_flow", flowId);
        FUSE_PROFILE_COUNTER("mixed_counter", 7);
        FUSE_PROFILE_ASYNC_FLOW_END("mixed_flow", flowId);
    }

    const std::string json = fuse::profiler::exportChromeTraceJson();
    expectTrue(json.find("\"ph\":\"B\"") != std::string::npos, "mixed export keeps scope begin");
    expectTrue(json.find("\"ph\":\"E\"") != std::string::npos, "mixed export keeps scope end");
    expectTrue(json.find("\"ph\":\"s\"") != std::string::npos, "mixed export keeps flow start");
    expectTrue(json.find("\"ph\":\"f\"") != std::string::npos, "mixed export keeps flow finish");
    expectTrue(json.find("\"ph\":\"C\"") != std::string::npos, "mixed export keeps counter sample");
    expectTrue(json.find("\"name\":\"mixed_parent\"") != std::string::npos,
               "mixed export keeps parent scope name");
    expectTrue(json.find("\"name\":\"mixed_flow\"") != std::string::npos,
               "mixed export keeps flow name");
    expectTrue(json.find("\"name\":\"mixed_counter\"") != std::string::npos,
               "mixed export keeps counter track name");
}

void testFatalHandlerHook() {
    resetState();

    struct FatalCaptureState {
        bool called = false;
        fuse::assertion::FatalContext context{};
    } captureState;

    fuse::assertion::setFatalHandler(
        [](const fuse::assertion::FatalContext& context, void* userData) {
            auto* state = static_cast<FatalCaptureState*>(userData);
            state->called = true;
            state->context = context;
        },
        &captureState);

    fuse::assertion::setSuppressAbortForTests(true);
    fuse::assertion::fatal("verify failed", "test_profiler_assert.cpp", 99u);

    expectTrue(captureState.called, "fatal handler invoked");
    expectTrue(captureState.context.message != nullptr
                   && std::string(captureState.context.message) == "verify failed",
               "fatal handler receives message");
    expectTrue(captureState.context.line == 99u, "fatal handler receives line");
}

void testHasEventsAndEmptyBufferGuards() {
    resetState();
    fuse::platform::registerMainThread();

    expectTrue(!fuse::profiler::hasEvents(), "reset leaves hasEvents false");
    expectTrue(fuse::profiler::eventCount() == 0u, "reset leaves event count at zero");
    expectTrue(!fuse::profiler::isEventIndexValid(0u), "isEventIndexValid false on empty buffer");
    expectTrue(!fuse::profiler::isEventIndexValid(99u), "isEventIndexValid false when out of range");

    const fuse::profiler::ProfileEvent& emptyEvent = fuse::profiler::eventAt(0);
    expectTrue(emptyEvent.name == nullptr, "eventAt on empty buffer returns sentinel with null name");
    expectTrue(!fuse::profiler::isValidProfileEvent(emptyEvent),
               "isValidProfileEvent false for empty-buffer sentinel");
    expectTrue(emptyEvent.phase == fuse::profiler::EventPhase::Begin,
               "eventAt sentinel keeps default begin phase");
    expectTrue(&emptyEvent == &fuse::profiler::emptyProfileEvent(),
               "eventAt sentinel matches emptyProfileEvent()");

    const fuse::profiler::ProfileEvent& oobEvent = fuse::profiler::eventAt(99);
    expectTrue(oobEvent.name == nullptr, "eventAt out-of-range returns sentinel with null name");
    expectTrue(!fuse::profiler::isValidProfileEvent(oobEvent),
               "isValidProfileEvent false for out-of-range sentinel");
    expectTrue(&oobEvent == &fuse::profiler::emptyProfileEvent(),
               "out-of-range eventAt returns emptyProfileEvent()");

    {
        FUSE_PROFILE_SCOPE("guard_scope");
    }

    expectTrue(fuse::profiler::hasEvents(), "hasEvents true after recording scope");
    expectTrue(fuse::profiler::isEventIndexValid(0u), "isEventIndexValid true for first event");
    expectTrue(fuse::profiler::isEventIndexValid(1u), "isEventIndexValid true for last event");
    expectTrue(!fuse::profiler::isEventIndexValid(2u), "isEventIndexValid false past event count");
    expectTrue(fuse::profiler::isValidProfileEvent(fuse::profiler::eventAt(0)),
               "isValidProfileEvent true for recorded event");
    expectTrue(fuse::profiler::eventAt(0).name != nullptr, "eventAt(0) valid after recording");
    expectTrue(fuse::profiler::eventAt(2).name == nullptr, "eventAt past count returns sentinel");
}

void testNestedAsyncFlowDepth() {
    resetState();
    fuse::platform::registerMainThread();

    const fuse::u32 outerFlowId = fuse::profiler::nextFlowId();
    const fuse::u32 innerFlowId = fuse::profiler::nextFlowId();

    FUSE_PROFILE_ASYNC_FLOW_BEGIN("outer_flow", outerFlowId);
    FUSE_PROFILE_ASYNC_FLOW_BEGIN("inner_flow", innerFlowId);
    FUSE_PROFILE_ASYNC_FLOW_END("inner_flow", innerFlowId);
    FUSE_PROFILE_ASYNC_FLOW_END("outer_flow", outerFlowId);

    expectTrue(fuse::profiler::eventCount() == 4u, "nested flows emit four events");
    expectTrue(fuse::profiler::maxFlowNestingDepth() == 2u, "max flow nesting depth tracks inner flow");

    const fuse::profiler::ProfileEvent& outerStart = fuse::profiler::eventAt(0);
    const fuse::profiler::ProfileEvent& innerStart = fuse::profiler::eventAt(1);
    const fuse::profiler::ProfileEvent& innerFinish = fuse::profiler::eventAt(2);
    const fuse::profiler::ProfileEvent& outerFinish = fuse::profiler::eventAt(3);

    expectTrue(outerStart.flowNestingDepth == 1u, "outer flow start records depth 1");
    expectTrue(innerStart.flowNestingDepth == 2u, "inner flow start records depth 2");
    expectTrue(innerFinish.flowNestingDepth == 2u, "inner flow finish records depth 2");
    expectTrue(outerFinish.flowNestingDepth == 1u, "outer flow finish records depth 1");

    const std::string json = fuse::profiler::exportChromeTraceJson();
    expectTrue(json.find("\"args\":{\"flow_depth\":1}") != std::string::npos,
               "chrome export includes outer flow depth");
    expectTrue(json.find("\"args\":{\"flow_depth\":2}") != std::string::npos,
               "chrome export includes inner flow depth");
}

void testCounterSnapshotAtFrame() {
    resetState();
    fuse::platform::registerMainThread();

    fuse::profiler::beginFrame();
    fuse::profiler::beginFrame();
    FUSE_PROFILE_COUNTER_SNAPSHOT_AT_FRAME("frame_budget", 8192);
    FUSE_PROFILE_COUNTER_SNAPSHOT_AT_FRAME("frame_time_ms", 12.5);

    expectTrue(fuse::profiler::eventCount() == 2u, "snapshot_at_frame counters emit two events");

    const fuse::profiler::ProfileEvent& intCounter = fuse::profiler::eventAt(0);
    const fuse::profiler::ProfileEvent& floatCounter = fuse::profiler::eventAt(1);
    expectTrue(intCounter.counterSnapshotFrame == 2u, "int counter records current frame index");
    expectTrue(floatCounter.counterSnapshotFrame == 2u, "float counter records current frame index");
    expectTrue(intCounter.counterIntValue == 8192, "int counter value preserved with frame snapshot");
    expectTrue(floatCounter.counterKind == fuse::profiler::CounterValueKind::Float,
               "float snapshot counter kind preserved");

    const std::string json = fuse::profiler::exportChromeTraceJson();
    expectTrue(json.find("\"args\":{\"value\":8192,\"snapshot_at_frame\":2}") != std::string::npos,
               "chrome export includes int counter snapshot_at_frame");
    expectTrue(json.find("\"args\":{\"value\":12.5,\"snapshot_at_frame\":2}") != std::string::npos,
               "chrome export includes float counter snapshot_at_frame");
}

void testCounterSnapshotAtFrameInsideScope() {
    resetState();
    fuse::platform::registerMainThread();

    fuse::profiler::beginFrame();
    {
        FUSE_PROFILE_SCOPE("budget_scope");
        FUSE_PROFILE_COUNTER_SNAPSHOT_AT_FRAME("scoped_frame_budget", 128);
    }

    const fuse::profiler::ProfileEvent& counter = fuse::profiler::eventAt(1);
    expectTrue(counter.nestingDepth == 1u, "snapshot counter inside scope inherits depth");
    expectTrue(counter.counterSnapshotFrame == 1u, "snapshot counter inside scope records frame index");

    const std::string json = fuse::profiler::exportChromeTraceJson();
    expectTrue(json.find("\"args\":{\"value\":128,\"depth\":1,\"snapshot_at_frame\":1}") != std::string::npos,
               "chrome export includes depth and snapshot_at_frame for scoped counter");
}

void testChromeTraceEscapedLowControlChars() {
    resetState();
    fuse::platform::registerMainThread();

    FUSE_PROFILE_COUNTER("bell\x07" "char", 1);

    const std::string json = fuse::profiler::exportChromeTraceJson();
    expectTrue(json.find("\"name\":\"bell\\u0007char\"") != std::string::npos,
               "chrome export escapes low control characters as unicode");
}

void testNestedFlowInsideScopeExportsBothDepths() {
    resetState();
    fuse::platform::registerMainThread();

    const fuse::u32 flowId = fuse::profiler::nextFlowId();
    {
        FUSE_PROFILE_SCOPE("flow_scope");
        FUSE_PROFILE_ASYNC_FLOW_BEGIN("scoped_flow", flowId);
        FUSE_PROFILE_ASYNC_FLOW_END("scoped_flow", flowId);
    }

    const fuse::profiler::ProfileEvent& flowStart = fuse::profiler::eventAt(1);
    expectTrue(flowStart.nestingDepth == 1u, "flow inside scope inherits scope depth");
    expectTrue(flowStart.flowNestingDepth == 1u, "flow inside scope records flow depth");

    const std::string json = fuse::profiler::exportChromeTraceJson();
    expectTrue(json.find("\"args\":{\"depth\":1,\"flow_depth\":1}") != std::string::npos,
               "chrome export includes scope and flow depth for nested flow");
}

void testDisabledProfilerDoesNotMutateFlowNestingDepth() {
    resetState();
    fuse::platform::registerMainThread();

    fuse::profiler::setEnabled(false);
    const fuse::u32 flowId = fuse::profiler::nextFlowId();
    FUSE_PROFILE_ASYNC_FLOW_BEGIN("ignored_outer", flowId);
    FUSE_PROFILE_ASYNC_FLOW_BEGIN("ignored_inner", flowId + 1u);
    FUSE_PROFILE_ASYNC_FLOW_END("ignored_inner", flowId + 1u);
    FUSE_PROFILE_ASYNC_FLOW_END("ignored_outer", flowId);

    expectTrue(fuse::profiler::eventCount() == 0u, "disabled profiler skips nested async flow events");
    expectTrue(fuse::profiler::maxFlowNestingDepth() == 0u,
               "disabled profiler does not mutate max flow nesting depth");

    fuse::profiler::setEnabled(true);
    FUSE_PROFILE_ASYNC_FLOW_BEGIN("enabled_flow", flowId);
    FUSE_PROFILE_ASYNC_FLOW_END("enabled_flow", flowId);
    expectTrue(fuse::profiler::maxFlowNestingDepth() == 1u,
               "flow nesting depth resumes cleanly after re-enable");
}

void testCounterInsideNestedFlowRecordsFlowDepth() {
    resetState();
    fuse::platform::registerMainThread();

    const fuse::u32 outerFlowId = fuse::profiler::nextFlowId();
    const fuse::u32 innerFlowId = fuse::profiler::nextFlowId();
    FUSE_PROFILE_ASYNC_FLOW_BEGIN("budget_outer", outerFlowId);
    FUSE_PROFILE_COUNTER("outer_budget", 10);
    FUSE_PROFILE_ASYNC_FLOW_BEGIN("budget_inner", innerFlowId);
    FUSE_PROFILE_COUNTER("inner_budget", 20);
    FUSE_PROFILE_ASYNC_FLOW_END("budget_inner", innerFlowId);
    FUSE_PROFILE_ASYNC_FLOW_END("budget_outer", outerFlowId);

    const fuse::profiler::ProfileEvent& outerCounter = fuse::profiler::eventAt(1);
    const fuse::profiler::ProfileEvent& innerCounter = fuse::profiler::eventAt(3);
    expectTrue(outerCounter.flowNestingDepth == 1u, "counter in outer flow records flow depth 1");
    expectTrue(innerCounter.flowNestingDepth == 2u, "counter in inner flow records flow depth 2");

    const std::string json = fuse::profiler::exportChromeTraceJson();
    expectTrue(json.find("\"name\":\"outer_budget\"") != std::string::npos,
               "nested flow counter export keeps outer track name");
    expectTrue(json.find("\"args\":{\"value\":10,\"flow_depth\":1}") != std::string::npos,
               "chrome export includes flow_depth for outer counter");
    expectTrue(json.find("\"args\":{\"value\":20,\"flow_depth\":2}") != std::string::npos,
               "chrome export includes flow_depth for inner counter");
}

void testSnapshotAtFrameCounterInsideNestedFlowAndScope() {
    resetState();
    fuse::platform::registerMainThread();

    fuse::profiler::beginFrame();
    fuse::profiler::beginFrame();
    const fuse::u32 flowId = fuse::profiler::nextFlowId();
    {
        FUSE_PROFILE_SCOPE("snapshot_scope");
        FUSE_PROFILE_ASYNC_FLOW_BEGIN("snapshot_flow", flowId);
        FUSE_PROFILE_COUNTER_SNAPSHOT_AT_FRAME("combined_budget", 256);
        FUSE_PROFILE_ASYNC_FLOW_END("snapshot_flow", flowId);
    }

    const fuse::profiler::ProfileEvent& counter = fuse::profiler::eventAt(2);
    expectTrue(counter.nestingDepth == 1u, "snapshot counter inherits scope depth");
    expectTrue(counter.flowNestingDepth == 1u, "snapshot counter inherits flow depth");
    expectTrue(counter.counterSnapshotFrame == 2u, "snapshot counter records frame index");

    const std::string json = fuse::profiler::exportChromeTraceJson();
    expectTrue(json.find("\"args\":{\"value\":256,\"depth\":1,\"flow_depth\":1,\"snapshot_at_frame\":2}")
                   != std::string::npos,
               "chrome export includes scope depth, flow depth, and snapshot_at_frame");
}

void testFlowNestingDepthIntrospection() {
    resetState();
    fuse::platform::registerMainThread();

    expectTrue(fuse::profiler::flowNestingDepth() == 0u, "reset leaves flow nesting depth at zero");

    const fuse::u32 outerFlowId = fuse::profiler::nextFlowId();
    const fuse::u32 innerFlowId = fuse::profiler::nextFlowId();
    FUSE_PROFILE_ASYNC_FLOW_BEGIN("depth_outer", outerFlowId);
    expectTrue(fuse::profiler::flowNestingDepth() == 1u, "flow begin increments introspection depth");
    FUSE_PROFILE_ASYNC_FLOW_BEGIN("depth_inner", innerFlowId);
    expectTrue(fuse::profiler::flowNestingDepth() == 2u, "nested flow begin increments introspection depth");
    FUSE_PROFILE_ASYNC_FLOW_END("depth_inner", innerFlowId);
    expectTrue(fuse::profiler::flowNestingDepth() == 1u, "inner flow end restores introspection depth");
    FUSE_PROFILE_ASYNC_FLOW_END("depth_outer", outerFlowId);
    expectTrue(fuse::profiler::flowNestingDepth() == 0u, "outer flow end clears introspection depth");
}

void testOrphanAsyncFlowEndIsIgnored() {
    resetState();
    fuse::platform::registerMainThread();

    expectTrue(fuse::profiler::flowNestingDepth() == 0u, "orphan guard starts with zero flow depth");
    FUSE_PROFILE_ASYNC_FLOW_END("orphan_finish", 99u);

    expectTrue(fuse::profiler::eventCount() == 0u, "orphan flow finish records no event");
    expectTrue(fuse::profiler::flowNestingDepth() == 0u, "orphan flow finish does not underflow depth");
    expectTrue(fuse::profiler::maxFlowNestingDepth() == 0u, "orphan flow finish does not bump max depth");

    const fuse::u32 flowId = fuse::profiler::nextFlowId();
    FUSE_PROFILE_ASYNC_FLOW_BEGIN("paired_flow", flowId);
    FUSE_PROFILE_ASYNC_FLOW_END("paired_flow", flowId);
    expectTrue(fuse::profiler::eventCount() == 2u, "paired flow still records after orphan guard");
}

void testNullNameFlowAndCounterGuards() {
    resetState();
    fuse::platform::registerMainThread();

    fuse::profiler::beginAsyncFlow(nullptr, 1u);
    fuse::profiler::endAsyncFlow(nullptr, 1u);
    fuse::profiler::sampleCounter(nullptr, 42);
    fuse::profiler::sampleCounterFloat(nullptr, 1.5);
    fuse::profiler::sampleCounterSnapshotAtFrame(nullptr, 7);
    fuse::profiler::sampleCounterFloatSnapshotAtFrame(nullptr, 0.25);

    expectTrue(fuse::profiler::eventCount() == 0u, "null flow/counter names record nothing");
    expectTrue(fuse::profiler::flowNestingDepth() == 0u, "null flow names do not mutate flow depth");
    expectTrue(fuse::profiler::exportChromeTraceJson().find("\"traceEvents\":[]") != std::string::npos,
               "null-name guard leaves export empty");
}

void testScopeNestingDepthIntrospection() {
    resetState();
    fuse::platform::registerMainThread();

    expectTrue(fuse::profiler::scopeNestingDepth() == 0u, "reset leaves scope nesting depth at zero");

    {
        FUSE_PROFILE_SCOPE("depth_outer");
        expectTrue(fuse::profiler::scopeNestingDepth() == 1u, "outer scope increments introspection depth");
        {
            FUSE_PROFILE_SCOPE("depth_inner");
            expectTrue(fuse::profiler::scopeNestingDepth() == 2u, "inner scope increments introspection depth");
        }
        expectTrue(fuse::profiler::scopeNestingDepth() == 1u, "inner scope end restores introspection depth");
    }
    expectTrue(fuse::profiler::scopeNestingDepth() == 0u, "outer scope end clears introspection depth");
}

void testOpenAsyncFlowCountTracking() {
    resetState();
    fuse::platform::registerMainThread();

    expectTrue(fuse::profiler::openAsyncFlowCount() == 0u, "reset leaves open async flow count at zero");

    const fuse::u32 outerFlowId = fuse::profiler::nextFlowId();
    const fuse::u32 innerFlowId = fuse::profiler::nextFlowId();
    FUSE_PROFILE_ASYNC_FLOW_BEGIN("count_outer", outerFlowId);
    expectTrue(fuse::profiler::openAsyncFlowCount() == 1u, "flow begin increments open count");
    FUSE_PROFILE_ASYNC_FLOW_BEGIN("count_inner", innerFlowId);
    expectTrue(fuse::profiler::openAsyncFlowCount() == 2u, "nested flow begin increments open count");
    FUSE_PROFILE_ASYNC_FLOW_END("count_inner", innerFlowId);
    expectTrue(fuse::profiler::openAsyncFlowCount() == 1u, "inner flow end decrements open count");
    FUSE_PROFILE_ASYNC_FLOW_END("count_outer", outerFlowId);
    expectTrue(fuse::profiler::openAsyncFlowCount() == 0u, "outer flow end clears open count");
}

void testLastEventIndexAndLastEvent() {
    resetState();
    fuse::platform::registerMainThread();

    expectTrue(fuse::profiler::lastEventIndex() == fuse::profiler::kInvalidEventIndex,
               "lastEventIndex is invalid on empty buffer");
    expectTrue(!fuse::profiler::isValidProfileEvent(fuse::profiler::lastEvent()),
               "lastEvent returns sentinel on empty buffer");

    {
        FUSE_PROFILE_SCOPE("first_scope");
    }
    expectTrue(fuse::profiler::lastEventIndex() == 1u, "lastEventIndex points at scope end");
    expectTrue(fuse::profiler::isValidProfileEvent(fuse::profiler::lastEvent()),
               "lastEvent is valid after recording");
    expectTrue(fuse::profiler::lastEvent().phase == fuse::profiler::EventPhase::End,
               "lastEvent returns most recent end phase");
    expectTrue(std::string(fuse::profiler::lastEvent().name) == "first_scope",
               "lastEvent preserves most recent scope name");
}

void testBufferEmptyAndFullGuards() {
    resetState();
    fuse::platform::registerMainThread();

    expectTrue(fuse::profiler::isBufferEmpty(), "reset leaves buffer empty");
    expectTrue(!fuse::profiler::isBufferFull(), "empty buffer is not full");
    expectTrue(!fuse::profiler::hasEvents(), "isBufferEmpty mirrors hasEvents on reset");

    {
        FUSE_PROFILE_SCOPE("single_scope");
    }

    expectTrue(!fuse::profiler::isBufferEmpty(), "recorded events clear isBufferEmpty");
    expectTrue(fuse::profiler::hasEvents(), "hasEvents true after recording");
    expectTrue(!fuse::profiler::isBufferFull(), "two events do not saturate ring buffer");
}

void testDisabledProfilerDoesNotMutateScopeNestingDepth() {
    resetState();
    fuse::platform::registerMainThread();

    fuse::profiler::setEnabled(false);
    {
        FUSE_PROFILE_SCOPE("ignored_outer");
        {
            FUSE_PROFILE_SCOPE("ignored_inner");
        }
    }

    expectTrue(fuse::profiler::eventCount() == 0u, "disabled profiler skips nested scope events");
    expectTrue(fuse::profiler::scopeNestingDepth() == 0u,
               "disabled profiler does not mutate scope nesting depth");
    expectTrue(fuse::profiler::maxNestingDepth() == 0u,
               "disabled profiler does not bump max scope nesting depth");

    fuse::profiler::setEnabled(true);
    {
        FUSE_PROFILE_SCOPE("enabled_scope");
    }
    expectTrue(fuse::profiler::maxNestingDepth() == 1u,
               "scope nesting depth resumes cleanly after re-enable");
}

void testDisabledAsyncFlowBeginSkipsDepthAndOpenCount() {
    resetState();
    fuse::platform::registerMainThread();

    fuse::profiler::setEnabled(false);
    const fuse::u32 flowId = fuse::profiler::nextFlowId();
    FUSE_PROFILE_ASYNC_FLOW_BEGIN("ignored_begin", flowId);

    expectTrue(fuse::profiler::flowNestingDepth() == 0u,
               "disabled flow begin does not mutate flow nesting depth");
    expectTrue(fuse::profiler::openAsyncFlowCount() == 0u,
               "disabled flow begin does not increment open async flow count");
    expectTrue(fuse::profiler::eventCount() == 0u, "disabled flow begin records no event");
}

void testNullScopeNameGuard() {
    resetState();
    fuse::platform::registerMainThread();

    {
        fuse::profiler::ProfileScope nullScope(nullptr);
    }

    expectTrue(fuse::profiler::eventCount() == 0u, "null scope name records nothing");
    expectTrue(fuse::profiler::scopeNestingDepth() == 0u, "null scope name does not mutate nesting depth");
    expectTrue(fuse::profiler::exportChromeTraceJson().find("\"traceEvents\":[]") != std::string::npos,
               "null scope name leaves export empty");
}

void testResetClearsOpenAsyncFlowCount() {
    resetState();
    fuse::platform::registerMainThread();

    const fuse::u32 flowId = fuse::profiler::nextFlowId();
    FUSE_PROFILE_ASYNC_FLOW_BEGIN("reset_flow", flowId);
    expectTrue(fuse::profiler::openAsyncFlowCount() == 1u, "open flow count tracks begin");

    fuse::profiler::reset();
    expectTrue(fuse::profiler::openAsyncFlowCount() == 0u, "reset clears open async flow count");
    expectTrue(fuse::profiler::isBufferEmpty(), "reset clears event buffer");
    expectTrue(fuse::profiler::flowNestingDepth() == 0u, "reset clears flow nesting depth");
}

void testDisabledCounterPreservesFlowNestingDepth() {
    resetState();
    fuse::platform::registerMainThread();

    const fuse::u32 flowId = fuse::profiler::nextFlowId();
    FUSE_PROFILE_ASYNC_FLOW_BEGIN("budget_flow", flowId);
    expectTrue(fuse::profiler::flowNestingDepth() == 1u, "flow begin establishes depth before disable");

    fuse::profiler::setEnabled(false);
    FUSE_PROFILE_COUNTER("ignored_in_flow", 11);
    FUSE_PROFILE_COUNTER_SNAPSHOT_AT_FRAME("ignored_snapshot", 22);
    fuse::profiler::setEnabled(true);

    expectTrue(fuse::profiler::flowNestingDepth() == 1u,
               "disabled counter samples do not mutate flow nesting depth");
    expectTrue(fuse::profiler::eventCount() == 1u, "only flow begin recorded before counter disable");

    FUSE_PROFILE_ASYNC_FLOW_END("budget_flow", flowId);
    expectTrue(fuse::profiler::flowNestingDepth() == 0u, "flow end restores depth after re-enable");
    expectTrue(fuse::profiler::eventCount() == 2u, "flow finish recorded after re-enable");
}

void testNestingDepthAliasIntrospection() {
    resetState();
    fuse::platform::registerMainThread();

    expectTrue(fuse::profiler::nestingDepth() == 0u, "reset leaves scope nesting depth at zero");

    {
        FUSE_PROFILE_SCOPE("depth_outer");
        expectTrue(fuse::profiler::nestingDepth() == 1u, "outer scope increments introspection depth");
        {
            FUSE_PROFILE_SCOPE("depth_inner");
            expectTrue(fuse::profiler::nestingDepth() == 2u, "inner scope increments introspection depth");
        }
        expectTrue(fuse::profiler::nestingDepth() == 1u, "inner scope end restores introspection depth");
    }
    expectTrue(fuse::profiler::nestingDepth() == 0u, "outer scope end clears introspection depth");
}

void testOpenAsyncFlowCountIntrospection() {
    resetState();
    fuse::platform::registerMainThread();

    expectTrue(fuse::profiler::openAsyncFlowCount() == 0u, "reset leaves open async flow count at zero");

    const fuse::u32 outerFlowId = fuse::profiler::nextFlowId();
    const fuse::u32 innerFlowId = fuse::profiler::nextFlowId();
    FUSE_PROFILE_ASYNC_FLOW_BEGIN("count_outer", outerFlowId);
    expectTrue(fuse::profiler::openAsyncFlowCount() == 1u, "flow begin increments open count");
    FUSE_PROFILE_ASYNC_FLOW_BEGIN("count_inner", innerFlowId);
    expectTrue(fuse::profiler::openAsyncFlowCount() == 2u, "nested flow begin increments open count");
    FUSE_PROFILE_ASYNC_FLOW_END("count_inner", innerFlowId);
    expectTrue(fuse::profiler::openAsyncFlowCount() == 1u, "inner flow end decrements open count");
    FUSE_PROFILE_ASYNC_FLOW_END("count_outer", outerFlowId);
    expectTrue(fuse::profiler::openAsyncFlowCount() == 0u, "outer flow end clears open count");
}

void testNullNameProfileScopeGuard() {
    resetState();
    fuse::platform::registerMainThread();

    {
        fuse::profiler::ProfileScope nullScope(nullptr);
    }

    expectTrue(fuse::profiler::eventCount() == 0u, "null scope name records nothing");
    expectTrue(fuse::profiler::nestingDepth() == 0u, "null scope name does not mutate nesting depth");
    expectTrue(fuse::profiler::maxNestingDepth() == 0u, "null scope name does not bump max nesting depth");
    expectTrue(fuse::profiler::exportChromeTraceJson().find("\"traceEvents\":[]") != std::string::npos,
               "null scope name leaves export empty");
}

void testResetClearsNestingAndFlowGuardState() {
    resetState();
    fuse::platform::registerMainThread();

    const fuse::u32 flowId = fuse::profiler::nextFlowId();
    {
        FUSE_PROFILE_SCOPE("reset_outer");
        FUSE_PROFILE_ASYNC_FLOW_BEGIN("reset_flow", flowId);
        FUSE_PROFILE_COUNTER("reset_counter", 5);
    }

    expectTrue(fuse::profiler::hasEvents(), "pre-reset profiler captured events");
    expectTrue(fuse::profiler::openAsyncFlowCount() == 1u,
               "pre-reset open flow count reflects unmatched begin");

    fuse::profiler::reset();

    expectTrue(!fuse::profiler::hasEvents(), "reset clears event buffer");
    expectTrue(fuse::profiler::nestingDepth() == 0u, "reset clears scope nesting depth");
    expectTrue(fuse::profiler::flowNestingDepth() == 0u, "reset clears flow nesting depth");
    expectTrue(fuse::profiler::openAsyncFlowCount() == 0u, "reset clears open async flow count");
    expectTrue(fuse::profiler::maxNestingDepth() == 0u, "reset clears max nesting depth");
    expectTrue(fuse::profiler::maxFlowNestingDepth() == 0u, "reset clears max flow nesting depth");
    expectTrue(fuse::profiler::exportChromeTraceJson().find("\"traceEvents\":[]") != std::string::npos,
               "reset leaves chrome export empty");
}

void testDisabledScopeDoesNotMutateNestingDepth() {
    resetState();
    fuse::platform::registerMainThread();

    fuse::profiler::setEnabled(false);
    {
        FUSE_PROFILE_SCOPE("ignored_outer");
        {
            FUSE_PROFILE_SCOPE("ignored_inner");
        }
    }

    expectTrue(fuse::profiler::eventCount() == 0u, "disabled profiler skips nested scope events");
    expectTrue(fuse::profiler::nestingDepth() == 0u,
               "disabled profiler does not mutate scope nesting depth");
    expectTrue(fuse::profiler::maxNestingDepth() == 0u,
               "disabled profiler does not bump max nesting depth");

    fuse::profiler::setEnabled(true);
    {
        FUSE_PROFILE_SCOPE("enabled_scope");
    }
    expectTrue(fuse::profiler::nestingDepth() == 0u, "scope nesting depth resumes cleanly after re-enable");
    expectTrue(fuse::profiler::maxNestingDepth() == 1u,
               "scope nesting depth resumes recording after re-enable");
}

void testMultipleOrphanAsyncFlowEndsAreIgnored() {
    resetState();
    fuse::platform::registerMainThread();

    FUSE_PROFILE_ASYNC_FLOW_END("orphan_a", 1u);
    FUSE_PROFILE_ASYNC_FLOW_END("orphan_b", 2u);
    FUSE_PROFILE_ASYNC_FLOW_END("orphan_c", 3u);

    expectTrue(fuse::profiler::eventCount() == 0u, "multiple orphan flow finishes record nothing");
    expectTrue(fuse::profiler::openAsyncFlowCount() == 0u, "orphan finishes do not underflow open count");
    expectTrue(fuse::profiler::flowNestingDepth() == 0u, "orphan finishes do not underflow flow depth");

    const fuse::u32 flowId = fuse::profiler::nextFlowId();
    FUSE_PROFILE_ASYNC_FLOW_BEGIN("paired_after_orphans", flowId);
    FUSE_PROFILE_ASYNC_FLOW_END("paired_after_orphans", flowId);
    expectTrue(fuse::profiler::eventCount() == 2u, "paired flow still records after orphan guards");
    expectTrue(fuse::profiler::openAsyncFlowCount() == 0u, "paired flow clears open count");
}

void testDisabledBeginAsyncFlowDoesNotIncrementOpenCount() {
    resetState();
    fuse::platform::registerMainThread();

    fuse::profiler::setEnabled(false);
    const fuse::u32 flowId = fuse::profiler::nextFlowId();
    FUSE_PROFILE_ASYNC_FLOW_BEGIN("ignored_begin", flowId);
    FUSE_PROFILE_ASYNC_FLOW_BEGIN("ignored_nested", flowId + 1u);

    expectTrue(fuse::profiler::eventCount() == 0u, "disabled flow begin records nothing");
    expectTrue(fuse::profiler::openAsyncFlowCount() == 0u,
               "disabled flow begin does not increment open count");
    expectTrue(!fuse::profiler::hasOpenAsyncFlows(),
               "hasOpenAsyncFlows false when disabled begin is skipped");
    expectTrue(fuse::profiler::flowNestingDepth() == 0u,
               "disabled flow begin does not mutate flow nesting depth");

    fuse::profiler::setEnabled(true);
    FUSE_PROFILE_ASYNC_FLOW_BEGIN("enabled_flow", flowId);
    expectTrue(fuse::profiler::openAsyncFlowCount() == 1u,
               "open count resumes cleanly after re-enable");
    expectTrue(fuse::profiler::hasOpenAsyncFlows(), "hasOpenAsyncFlows true after enabled begin");
    FUSE_PROFILE_ASYNC_FLOW_END("enabled_flow", flowId);
    expectTrue(fuse::profiler::openAsyncFlowCount() == 0u, "enabled flow pair clears open count");
    expectTrue(!fuse::profiler::hasOpenAsyncFlows(), "hasOpenAsyncFlows false after flow pair");
}

void testRingCapacityAndBufferFullGuards() {
    resetState();
    fuse::platform::registerMainThread();

    expectTrue(fuse::profiler::ringCapacity() > 0u, "ringCapacity exposes non-zero capacity");
    expectTrue(fuse::profiler::eventCount() < fuse::profiler::ringCapacity(),
               "empty buffer is below ring capacity");
    expectTrue(!fuse::profiler::isBufferFull(), "empty buffer is not full");

    {
        FUSE_PROFILE_SCOPE("capacity_probe");
    }

    expectTrue(!fuse::profiler::isBufferFull(), "few events do not saturate ring buffer");
    expectTrue(fuse::profiler::eventCount() <= fuse::profiler::ringCapacity(),
               "event count stays within ring capacity");
}

void testDisabledAsyncFlowEndPreservesOpenCount() {
    resetState();
    fuse::platform::registerMainThread();

    const fuse::u32 flowId = fuse::profiler::nextFlowId();
    FUSE_PROFILE_ASYNC_FLOW_BEGIN("disabled_end_flow", flowId);
    expectTrue(fuse::profiler::openAsyncFlowCount() == 1u, "flow begin establishes open count");
    expectTrue(fuse::profiler::hasOpenAsyncFlows(), "hasOpenAsyncFlows true after begin");

    fuse::profiler::setEnabled(false);
    FUSE_PROFILE_ASYNC_FLOW_END("disabled_end_flow", flowId);
    expectTrue(fuse::profiler::openAsyncFlowCount() == 1u,
               "disabled flow finish preserves open async flow count");
    expectTrue(fuse::profiler::hasOpenAsyncFlows(),
               "hasOpenAsyncFlows remains true after disabled finish");
    expectTrue(fuse::profiler::flowNestingDepth() == 1u,
               "disabled flow finish preserves thread-local flow depth");
    expectTrue(fuse::profiler::eventCount() == 1u, "disabled finish does not record flow finish event");

    fuse::profiler::setEnabled(true);
    FUSE_PROFILE_ASYNC_FLOW_END("disabled_end_flow", flowId);
    expectTrue(fuse::profiler::openAsyncFlowCount() == 0u,
               "re-enabled flow finish clears open async flow count");
    expectTrue(!fuse::profiler::hasOpenAsyncFlows(), "hasOpenAsyncFlows false after re-enabled finish");
    expectTrue(fuse::profiler::flowNestingDepth() == 0u,
               "re-enabled flow finish restores thread-local flow depth");
    expectTrue(fuse::profiler::eventCount() == 2u, "paired flow events recorded after disable guard");
}

void testCrossThreadFlowFinishSkipsWorkerFlowDepthPop() {
    resetState();
    fuse::platform::registerMainThread();

    const fuse::u32 flowId = fuse::profiler::nextFlowId();
    FUSE_PROFILE_ASYNC_FLOW_BEGIN("cross_thread_flow", flowId);
    expectTrue(fuse::profiler::flowNestingDepth() == 1u, "begin thread records flow depth");
    expectTrue(fuse::profiler::hasOpenAsyncFlows(), "cross-thread flow begin marks open async flow");

    std::atomic<bool> workerDone{false};
    std::thread worker([&]() {
        expectTrue(fuse::profiler::flowNestingDepth() == 0u,
                   "worker thread starts with zero flow depth");
        FUSE_PROFILE_ASYNC_FLOW_END("cross_thread_flow", flowId);
        expectTrue(fuse::profiler::flowNestingDepth() == 0u,
                   "cross-thread flow finish does not underflow worker flow depth");
        workerDone.store(true, std::memory_order_release);
    });
    worker.join();
    expectTrue(workerDone.load(std::memory_order_acquire), "worker thread completed");

    expectTrue(fuse::profiler::openAsyncFlowCount() == 0u,
               "cross-thread flow finish clears global open count");
    expectTrue(!fuse::profiler::hasOpenAsyncFlows(),
               "hasOpenAsyncFlows false after cross-thread finish");
    expectTrue(fuse::profiler::flowNestingDepth() == 1u,
               "begin thread flow depth remains after cross-thread finish stub");
    expectTrue(fuse::profiler::eventCount() == 2u, "cross-thread flow records start and finish");

    fuse::profiler::reset();
    expectTrue(fuse::profiler::flowNestingDepth() == 0u,
               "reset clears begin-thread flow depth after cross-thread handoff");
}

void testDisabledScopeDoesNotMutateScopeNestingDepth() {
    resetState();
    fuse::platform::registerMainThread();

    fuse::profiler::setEnabled(false);
    {
        FUSE_PROFILE_SCOPE("ignored_scope");
        expectTrue(fuse::profiler::scopeNestingDepth() == 0u,
                   "disabled scope does not mutate scope nesting depth");
        expectTrue(fuse::profiler::nestingDepth() == 0u,
                   "disabled scope does not mutate nestingDepth introspection");
    }
    expectTrue(fuse::profiler::scopeNestingDepth() == 0u,
               "disabled scope destructor does not underflow scope depth");
    expectTrue(fuse::profiler::maxNestingDepth() == 0u,
               "disabled scope does not bump max nesting depth");

    fuse::profiler::setEnabled(true);
    {
        FUSE_PROFILE_SCOPE("enabled_scope");
        expectTrue(fuse::profiler::scopeNestingDepth() == 1u, "scope nesting resumes cleanly after re-enable");
        expectTrue(fuse::profiler::nestingDepth() == 1u, "nestingDepth mirrors scope depth after re-enable");
    }
    expectTrue(fuse::profiler::scopeNestingDepth() == 0u, "enabled scope restores depth after re-enable");
}

void testResetClearsNestingAndOpenFlowState() {
    resetState();
    fuse::platform::registerMainThread();

    const fuse::u32 flowId = fuse::profiler::nextFlowId();
    {
        FUSE_PROFILE_SCOPE("reset_outer");
        FUSE_PROFILE_ASYNC_FLOW_BEGIN("reset_flow", flowId);
        FUSE_PROFILE_COUNTER("reset_counter", 5);
    }

    expectTrue(fuse::profiler::scopeNestingDepth() == 0u, "scope ends before reset check");
    expectTrue(fuse::profiler::openAsyncFlowCount() == 1u, "open flow survives scope exit until reset");
    expectTrue(fuse::profiler::hasOpenAsyncFlows(), "hasOpenAsyncFlows true before reset");
    expectTrue(fuse::profiler::hasEvents(), "events remain before reset");

    fuse::profiler::reset();

    expectTrue(fuse::profiler::openAsyncFlowCount() == 0u, "reset clears open async flow count");
    expectTrue(!fuse::profiler::hasOpenAsyncFlows(), "hasOpenAsyncFlows false after reset");
    expectTrue(fuse::profiler::scopeNestingDepth() == 0u, "reset clears scope nesting depth");
    expectTrue(fuse::profiler::flowNestingDepth() == 0u, "reset clears flow nesting depth");
    expectTrue(!fuse::profiler::hasEvents(), "reset clears event buffer");
    expectTrue(fuse::profiler::isBufferEmpty(), "reset leaves buffer empty");
    expectTrue(!fuse::profiler::isEventIndexValid(0u), "reset leaves isEventIndexValid false");
    expectTrue(fuse::profiler::exportChromeTraceJson().find("\"traceEvents\":[]") != std::string::npos,
               "reset leaves empty chrome export");
}

void testDisabledProfilerSkipsSnapshotCounter() {
    resetState();
    fuse::platform::registerMainThread();

    fuse::profiler::beginFrame();
    fuse::profiler::setEnabled(false);
    FUSE_PROFILE_COUNTER_SNAPSHOT_AT_FRAME("ignored_snapshot", 512);
    FUSE_PROFILE_COUNTER_SNAPSHOT_AT_FRAME("ignored_float_snapshot", 1.25);

    expectTrue(fuse::profiler::eventCount() == 0u,
               "disabled profiler skips snapshot_at_frame counter samples");
    expectTrue(fuse::profiler::isBufferEmpty(), "disabled snapshot counter leaves buffer empty");
    expectTrue(fuse::profiler::exportChromeTraceJson().find("\"traceEvents\":[]") != std::string::npos,
               "disabled snapshot counter export stays empty");
}

void testVerifyMacro() {
    resetState();
    fuse::assertion::setSuppressAbortForTests(true);

    bool handlerCalled = false;
    fuse::assertion::setFatalHandler(
        [](const fuse::assertion::FatalContext&, void* userData) {
            *static_cast<bool*>(userData) = true;
        },
        &handlerCalled);

    FUSE_VERIFY(1 + 1 == 2, "math still works");
    expectTrue(!handlerCalled, "verify does not fire on true condition");

    FUSE_VERIFY(false, "expected failure");
    expectTrue(handlerCalled, "verify fires fatal handler on false condition");
}

} // namespace

int main() {
    testProfileScopeRecordsEvents();
    testProfilerDisabledSkipsEvents();
    testFrameBoundaryIncrementsIndex();
    testNestedScopeOrdering();
    testThreadIdStubs();
    testChromeTraceExport();
    testAsyncFlowStubs();
    testAsyncFlowCrossThread();
    testCounterSamples();
    testFloatCounterSamples();
    testAsyncFlowMatchingIdsInExport();
    testChromeTraceNestingDepthExport();
    testChromeTraceEmptyExport();
    testNestedAsyncFlowWithinScope();
    testDisabledProfilerSkipsAsyncFlowAndCounter();
    testChromeTraceEscapedNameExport();
    testChromeTraceEscapedControlCharsExport();
    testChromeTraceEscapedScopeAndFlowNames();
    testCounterSampleInsideScopeDepthExport();
    testMultipleAsyncFlowsInNestedScopes();
    testChromeTraceExportFrameIndex();
    testChromeTraceExportMixedEvents();
    testHasEventsAndEmptyBufferGuards();
    testNestedAsyncFlowDepth();
    testCounterSnapshotAtFrame();
    testCounterSnapshotAtFrameInsideScope();
    testChromeTraceEscapedLowControlChars();
    testNestedFlowInsideScopeExportsBothDepths();
    testDisabledProfilerDoesNotMutateFlowNestingDepth();
    testCounterInsideNestedFlowRecordsFlowDepth();
    testSnapshotAtFrameCounterInsideNestedFlowAndScope();
    testFlowNestingDepthIntrospection();
    testScopeNestingDepthIntrospection();
    testOpenAsyncFlowCountTracking();
    testLastEventIndexAndLastEvent();
    testBufferEmptyAndFullGuards();
    testOrphanAsyncFlowEndIsIgnored();
    testNullNameFlowAndCounterGuards();
    testNullScopeNameGuard();
    testDisabledProfilerDoesNotMutateScopeNestingDepth();
    testDisabledAsyncFlowBeginSkipsDepthAndOpenCount();
    testResetClearsOpenAsyncFlowCount();
    testDisabledCounterPreservesFlowNestingDepth();
    testNestingDepthAliasIntrospection();
    testOpenAsyncFlowCountIntrospection();
    testNullNameProfileScopeGuard();
    testResetClearsNestingAndFlowGuardState();
    testDisabledScopeDoesNotMutateNestingDepth();
    testMultipleOrphanAsyncFlowEndsAreIgnored();
    testDisabledBeginAsyncFlowDoesNotIncrementOpenCount();
    testRingCapacityAndBufferFullGuards();
    testDisabledAsyncFlowEndPreservesOpenCount();
    testCrossThreadFlowFinishSkipsWorkerFlowDepthPop();
    testDisabledScopeDoesNotMutateScopeNestingDepth();
    testResetClearsNestingAndOpenFlowState();
    testDisabledProfilerSkipsSnapshotCounter();
    testFatalHandlerHook();
    testVerifyMacro();

    if (g_failures != 0) {
        std::fprintf(stderr, "%d test(s) failed\n", g_failures);
        return EXIT_FAILURE;
    }

    std::fprintf(stdout, "fuse_core_profiler_assert: all tests passed\n");
    return EXIT_SUCCESS;
}
