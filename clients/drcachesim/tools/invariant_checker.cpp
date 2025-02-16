/* **********************************************************
 * Copyright (c) 2017-2023 Google, Inc.  All rights reserved.
 * **********************************************************/

/*
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * * Redistributions of source code must retain the above copyright notice,
 *   this list of conditions and the following disclaimer.
 *
 * * Redistributions in binary form must reproduce the above copyright notice,
 *   this list of conditions and the following disclaimer in the documentation
 *   and/or other materials provided with the distribution.
 *
 * * Neither the name of Google, Inc. nor the names of its contributors may be
 *   used to endorse or promote products derived from this software without
 *   specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL VMWARE, INC. OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH
 * DAMAGE.
 */

#include "dr_api.h"
#include "invariant_checker.h"
#include "invariant_checker_create.h"
#include <algorithm>
#include <iostream>
#include <cassert>
#include <string.h>

analysis_tool_t *
invariant_checker_create(bool offline, unsigned int verbose)
{
    return new invariant_checker_t(offline, verbose, "");
}

invariant_checker_t::invariant_checker_t(bool offline, unsigned int verbose,
                                         std::string test_name,
                                         std::istream *serial_schedule_file,
                                         std::istream *cpu_schedule_file)
    : knob_offline_(offline)
    , knob_verbose_(verbose)
    , knob_test_name_(test_name)
    , serial_schedule_file_(serial_schedule_file)
    , cpu_schedule_file_(cpu_schedule_file)
{
    if (knob_test_name_ == "kernel_xfer_app" || knob_test_name_ == "rseq_app")
        has_annotations_ = true;
}

invariant_checker_t::~invariant_checker_t()
{
}

std::string
invariant_checker_t::initialize_stream(memtrace_stream_t *serial_stream)
{
    serial_stream_ = serial_stream;
    return "";
}

void
invariant_checker_t::report_if_false(per_shard_t *shard, bool condition,
                                     const std::string &invariant_name)
{
    if (!condition) {
        std::cerr << "Trace invariant failure in T" << shard->tid_ << " at ref # "
                  << shard->stream->get_record_ordinal() << ": " << invariant_name
                  << "\n";
        abort();
    }
}

bool
invariant_checker_t::parallel_shard_supported()
{
    return true;
}

void *
invariant_checker_t::parallel_shard_init_stream(int shard_index, void *worker_data,
                                                memtrace_stream_t *shard_stream)
{
    auto per_shard = std::unique_ptr<per_shard_t>(new per_shard_t);
    per_shard->stream = shard_stream;
    void *res = reinterpret_cast<void *>(per_shard.get());
    std::lock_guard<std::mutex> guard(shard_map_mutex_);
    shard_map_[shard_index] = std::move(per_shard);
    return res;
}

// We have no stream interface in invariant_checker_test unit tests.
// XXX: Could we refactor the test to use a reader that takes a vector?
void *
invariant_checker_t::parallel_shard_init(int shard_index, void *worker_data)
{
    return parallel_shard_init_stream(shard_index, worker_data, nullptr);
}

bool
invariant_checker_t::parallel_shard_exit(void *shard_data)
{
    return true;
}

std::string
invariant_checker_t::parallel_shard_error(void *shard_data)
{
    per_shard_t *shard = reinterpret_cast<per_shard_t *>(shard_data);
    return shard->error_;
}

bool
invariant_checker_t::parallel_shard_memref(void *shard_data, const memref_t &memref)
{
    per_shard_t *shard = reinterpret_cast<per_shard_t *>(shard_data);
    if (shard->tid_ == -1 && memref.data.tid != 0)
        shard->tid_ = memref.data.tid;
    // We check the memtrace_stream_t counts with our own, unless there was an
    // instr skip from the start where we cannot compare, or we're in a unit
    // test with no stream interface, or we're in serial mode (since we want
    // per-shard counts for error reporting; XXX: we could add our own global
    // counts to compare to the serial stream).
    ++shard->ref_count_;
    if (type_is_instr(memref.instr.type))
        ++shard->instr_count_;
    // XXX: We also can't verify counts with a skip invoked from the middle, but
    // we have no simple way to detect that here.
    if (shard->instr_count_ <= 1 && !shard->skipped_instrs_ && shard->stream != nullptr &&
        shard->stream->get_instruction_ordinal() > 1)
        shard->skipped_instrs_ = true;
    if (!shard->skipped_instrs_ && shard->stream != nullptr &&
        (shard->stream != serial_stream_ || shard_map_.size() == 1)) {
        report_if_false(shard, shard->ref_count_ == shard->stream->get_record_ordinal(),
                        "Stream record ordinal inaccurate");
        report_if_false(shard,
                        shard->instr_count_ == shard->stream->get_instruction_ordinal(),
                        "Stream instr ordinal inaccurate");
    }
#ifdef UNIX
    if (has_annotations_) {
        // Check conditions specific to the signal_invariants app, where it
        // has annotations in prefetch instructions telling us how many instrs
        // and/or memrefs until a signal should arrive.
        if ((shard->instrs_until_interrupt_ == 0 &&
             shard->memrefs_until_interrupt_ == -1) ||
            (shard->instrs_until_interrupt_ == -1 &&
             shard->memrefs_until_interrupt_ == 0) ||
            (shard->instrs_until_interrupt_ == 0 &&
             shard->memrefs_until_interrupt_ == 0)) {
            report_if_false(
                shard,
                (memref.marker.type == TRACE_TYPE_MARKER &&
                 (memref.marker.marker_type == TRACE_MARKER_TYPE_KERNEL_EVENT ||
                  memref.marker.marker_type == TRACE_MARKER_TYPE_RSEQ_ABORT)) ||
                    // TODO i#3937: Online instr bundles currently violate this.
                    !knob_offline_,
                "Interruption marker mis-placed");
            shard->instrs_until_interrupt_ = -1;
            shard->memrefs_until_interrupt_ = -1;
        }
        if (shard->memrefs_until_interrupt_ >= 0 &&
            (memref.data.type == TRACE_TYPE_READ ||
             memref.data.type == TRACE_TYPE_WRITE)) {
            report_if_false(shard, shard->memrefs_until_interrupt_ != 0,
                            "Interruption marker too late");
            --shard->memrefs_until_interrupt_;
        }
        // Check that the signal delivery marker is immediately followed by the
        // app's signal handler, which we have annotated with "prefetcht0 [1]".
        if (memref.data.type == TRACE_TYPE_PREFETCHT0 && memref.data.addr == 1) {
            report_if_false(shard,
                            type_is_instr(shard->prev_entry_.instr.type) &&
                                shard->prev_prev_entry_.marker.type ==
                                    TRACE_TYPE_MARKER &&
                                shard->last_xfer_marker_.marker.marker_type ==
                                    TRACE_MARKER_TYPE_KERNEL_EVENT,
                            "Signal handler not immediately after signal marker");
            shard->app_handler_pc_ = shard->prev_entry_.instr.addr;
        }
        // Look for annotations where signal_invariants.c and rseq.c pass info to us on
        // what to check for.  We assume the app does not have prefetch instrs w/ low
        // addresses.
        if (memref.data.type == TRACE_TYPE_PREFETCHT2 && memref.data.addr < 1024) {
            shard->instrs_until_interrupt_ = static_cast<int>(memref.data.addr);
        }
        if (memref.data.type == TRACE_TYPE_PREFETCHT1 && memref.data.addr < 1024) {
            shard->memrefs_until_interrupt_ = static_cast<int>(memref.data.addr);
        }
    }
    if (memref.marker.type == TRACE_TYPE_MARKER &&
        shard->prev_entry_.marker.type == TRACE_TYPE_MARKER &&
        shard->prev_entry_.marker.marker_type == TRACE_MARKER_TYPE_RSEQ_ABORT) {
        // The rseq marker must be immediately prior to the kernel event marker.
        report_if_false(shard,
                        memref.marker.marker_type == TRACE_MARKER_TYPE_KERNEL_EVENT,
                        "Rseq marker not immediately prior to kernel marker");
    }
    if (memref.marker.type == TRACE_TYPE_MARKER &&
        memref.marker.marker_type == TRACE_MARKER_TYPE_RSEQ_ENTRY) {
        shard->in_rseq_region_ = true;
        shard->rseq_start_pc_ = 0;
        shard->rseq_end_pc_ = memref.marker.marker_value;
    } else if (shard->in_rseq_region_) {
        if (type_is_instr(memref.instr.type)) {
            if (shard->rseq_start_pc_ == 0)
                shard->rseq_start_pc_ = memref.instr.addr;
            if (memref.instr.addr + memref.instr.size == shard->rseq_end_pc_) {
                // Completed normally.
                shard->in_rseq_region_ = false;
            } else if (memref.instr.addr >= shard->rseq_start_pc_ &&
                       memref.instr.addr < shard->rseq_end_pc_) {
                // Still in the region.
            } else {
                // We should see an abort marker or a side exit if we leave the region.
                report_if_false(shard,
                                type_is_instr_branch(shard->prev_instr_.instr.type),
                                "Rseq region exit requires marker, branch, or commit");
                shard->in_rseq_region_ = false;
            }
        } else {
            report_if_false(shard,
                            memref.marker.type != TRACE_TYPE_MARKER ||
                                memref.marker.marker_type !=
                                    TRACE_MARKER_TYPE_KERNEL_EVENT ||
                                // Side exit.
                                type_is_instr_branch(shard->prev_instr_.instr.type),
                            "Signal in rseq region should have abort marker");
        }
    }
    if (memref.marker.type == TRACE_TYPE_MARKER &&
        memref.marker.marker_type == TRACE_MARKER_TYPE_RSEQ_ABORT) {
        // Check that the rseq final instruction was not executed: that raw2trace
        // rolled it back, unless it was a fault in the instrumented execution in which
        // case the marker value will point to it.
        report_if_false(shard,
                        shard->rseq_end_pc_ == 0 ||
                            shard->prev_instr_.instr.addr +
                                    shard->prev_instr_.instr.size !=
                                shard->rseq_end_pc_ ||
                            shard->prev_instr_.instr.addr == memref.marker.marker_value,
                        "Rseq post-abort instruction not rolled back");
        shard->in_rseq_region_ = false;
    }
#endif

    if (memref.marker.type == TRACE_TYPE_MARKER &&
        memref.marker.marker_type == TRACE_MARKER_TYPE_FILETYPE) {
        shard->file_type_ = static_cast<offline_file_type_t>(memref.marker.marker_value);
        report_if_false(shard,
                        shard->stream == nullptr ||
                            shard->file_type_ == shard->stream->get_filetype(),
                        "Stream interface filetype != trace marker");
    }
    if (memref.marker.type == TRACE_TYPE_MARKER &&
        memref.marker.marker_type == TRACE_MARKER_TYPE_INSTRUCTION_COUNT) {
        shard->found_instr_count_marker_ = true;
        report_if_false(shard,
                        memref.marker.marker_value >= shard->last_instr_count_marker_,
                        "Instr count markers not increasing");
        shard->last_instr_count_marker_ = memref.marker.marker_value;
    }
    if (memref.marker.type == TRACE_TYPE_MARKER &&
        memref.marker.marker_type == TRACE_MARKER_TYPE_CACHE_LINE_SIZE) {
        shard->found_cache_line_size_marker_ = true;
        report_if_false(shard,
                        shard->stream == nullptr ||
                            memref.marker.marker_value ==
                                shard->stream->get_cache_line_size(),
                        "Stream interface cache line size != trace marker");
    }
    if (memref.marker.type == TRACE_TYPE_MARKER &&
        memref.marker.marker_type == TRACE_MARKER_TYPE_PAGE_SIZE) {
        shard->found_page_size_marker_ = true;
        report_if_false(shard,
                        shard->stream == nullptr ||
                            memref.marker.marker_value == shard->stream->get_page_size(),
                        "Stream interface page size != trace marker");
    }
    if (memref.marker.type == TRACE_TYPE_MARKER &&
        memref.marker.marker_type == TRACE_MARKER_TYPE_VERSION) {
        report_if_false(shard,
                        shard->stream == nullptr ||
                            memref.marker.marker_value == shard->stream->get_version(),
                        "Stream interface version != trace marker");
    }
    // Ensure each syscall instruction has a marker immediately afterward.  An
    // asynchronous signal could be delivered after the tracer recorded the syscall
    // instruction but before DR executed the syscall itself (xref i#5790) but raw2trace
    // removes the syscall instruction in such cases.
    if (memref.marker.type == TRACE_TYPE_MARKER &&
        memref.marker.marker_type == TRACE_MARKER_TYPE_SYSCALL) {
        shard->found_syscall_marker_ = true;
        ++shard->syscall_count_;
        // TODO i#5949: For WOW64 instr_is_syscall() always returns false here as it
        // tries to check adjacent instrs; we disable this check until that is solved.
#if !defined(WINDOWS) || defined(X64)
        if (shard->prev_instr_decoded_ != nullptr) {
            report_if_false(shard, instr_is_syscall(shard->prev_instr_decoded_->data),
                            "Syscall marker not placed after syscall instruction");
        }
#endif
    } else if (TESTANY(OFFLINE_FILE_TYPE_SYSCALL_NUMBERS, shard->file_type_) &&
               type_is_instr(shard->prev_entry_.instr.type) &&
               shard->prev_instr_decoded_ != nullptr &&
               // TODO i#5949: For WOW64 instr_is_syscall() always returns false.
               instr_is_syscall(shard->prev_instr_decoded_->data)) {
        report_if_false(shard,
                        shard->found_syscall_marker_ &&
                            shard->prev_entry_.marker.type == TRACE_TYPE_MARKER &&
                            shard->prev_entry_.marker.marker_type ==
                                TRACE_MARKER_TYPE_SYSCALL,
                        "Syscall instruction not followed by syscall marker");
    }
    if (memref.marker.type == TRACE_TYPE_MARKER &&
        memref.marker.marker_type == TRACE_MARKER_TYPE_MAYBE_BLOCKING_SYSCALL) {
        shard->found_blocking_marker_ = true;
        report_if_false(shard,
                        shard->prev_entry_.marker.type == TRACE_TYPE_MARKER &&
                            shard->prev_entry_.marker.marker_type ==
                                TRACE_MARKER_TYPE_SYSCALL,
                        "Maybe-blocking marker not preceded by syscall marker");
    }

    // Invariant: each chunk's instruction count must be identical and equal to
    // the value in the top-level marker.
    if (memref.marker.type == TRACE_TYPE_MARKER &&
        memref.marker.marker_type == TRACE_MARKER_TYPE_CHUNK_INSTR_COUNT) {
        shard->chunk_instr_count_ = memref.marker.marker_value;
        report_if_false(shard,
                        shard->stream == nullptr ||
                            shard->chunk_instr_count_ ==
                                shard->stream->get_chunk_instr_count(),
                        "Stream interface chunk instr count != trace marker");
    }
    if (memref.marker.type == TRACE_TYPE_MARKER &&
        memref.marker.marker_type == TRACE_MARKER_TYPE_CHUNK_FOOTER) {
        report_if_false(shard,
                        shard->skipped_instrs_ ||
                            (shard->chunk_instr_count_ != 0 &&
                             shard->instr_count_ % shard->chunk_instr_count_ == 0),
                        "Chunk instruction counts are inconsistent");
    }

    // Invariant: a function marker should not appear between an instruction and its
    // memrefs or in the middle of a block (we assume elision is turned off and so a
    // callee entry will always be the top of a block).  (We don't check for other types
    // of markers b/c a virtual2physical one *could* appear in between.)
    if (shard->prev_entry_.marker.type == TRACE_TYPE_MARKER &&
        marker_type_is_function_marker(shard->prev_entry_.marker.marker_type)) {
        report_if_false(shard,
                        memref.data.type != TRACE_TYPE_READ &&
                            memref.data.type != TRACE_TYPE_WRITE &&
                            !type_is_prefetch(memref.data.type),
                        "Function marker misplaced between instr and memref");
    }
    if (memref.marker.type == TRACE_TYPE_MARKER &&
        memref.marker.marker_type == TRACE_MARKER_TYPE_FUNC_ID) {
        shard->prev_func_id_ = memref.marker.marker_value;
    }
    if (memref.marker.type == TRACE_TYPE_MARKER &&
        marker_type_is_function_marker(memref.marker.marker_type)) {
        report_if_false(shard,
                        shard->prev_func_id_ >= TRACE_FUNC_ID_SYSCALL_BASE ||
                            type_is_instr_branch(shard->prev_instr_.instr.type),
                        "Function marker should be after a branch");
    }
    if (memref.marker.type == TRACE_TYPE_MARKER &&
        memref.marker.marker_type == TRACE_MARKER_TYPE_FUNC_RETADDR) {
        report_if_false(shard, memref.marker.marker_value == shard->last_retaddr_,
                        "Function marker retaddr should match prior call");
    }

    if (memref.exit.type == TRACE_TYPE_THREAD_EXIT) {
        report_if_false(shard,
                        !TESTANY(OFFLINE_FILE_TYPE_FILTERED | OFFLINE_FILE_TYPE_IFILTERED,
                                 shard->file_type_) ||
                            shard->found_instr_count_marker_,
                        "Missing instr count markers");
        report_if_false(shard,
                        shard->found_cache_line_size_marker_ ||
                            (shard->skipped_instrs_ && shard->stream != nullptr &&
                             shard->stream->get_cache_line_size() > 0),
                        "Missing cache line marker");
        report_if_false(shard,
                        shard->found_page_size_marker_ ||
                            (shard->skipped_instrs_ && shard->stream != nullptr &&
                             shard->stream->get_page_size() > 0),
                        "Missing page size marker");
        report_if_false(
            shard,
            shard->found_syscall_marker_ ==
                    // Making sure this is a bool for a safe comparison.
                    static_cast<bool>(
                        TESTANY(OFFLINE_FILE_TYPE_SYSCALL_NUMBERS, shard->file_type_)) ||
                shard->syscall_count_ == 0,
            "System call numbers presence does not match filetype");
        // We can't easily identify blocking syscalls ourselves so we only check
        // one direction here.
        report_if_false(
            shard,
            !shard->found_blocking_marker_ ||
                TESTANY(OFFLINE_FILE_TYPE_BLOCKING_SYSCALLS, shard->file_type_),
            "Kernel scheduling marker presence does not match filetype");
        if (knob_test_name_ == "filter_asm_instr_count") {
            static constexpr int ASM_INSTR_COUNT = 133;
            report_if_false(shard, shard->last_instr_count_marker_ == ASM_INSTR_COUNT,
                            "Incorrect instr count marker value");
        }
    }
    if (shard->prev_entry_.marker.type == TRACE_TYPE_MARKER &&
        shard->prev_entry_.marker.marker_type == TRACE_MARKER_TYPE_PHYSICAL_ADDRESS) {
        // A physical address marker must be immediately prior to its virtual marker.
        report_if_false(shard,
                        memref.marker.type == TRACE_TYPE_MARKER &&
                            memref.marker.marker_type ==
                                TRACE_MARKER_TYPE_VIRTUAL_ADDRESS,
                        "Physical addr marker not immediately prior to virtual marker");
        // We don't have the actual page size, but it is always at least 4K, so
        // make sure the bottom 12 bits are the same.
        report_if_false(shard,
                        (memref.marker.marker_value & 0xfff) ==
                            (shard->prev_entry_.marker.marker_value & 0xfff),
                        "Physical addr bottom 12 bits do not match virtual");
    }
    if (type_is_instr(memref.instr.type) ||
        memref.instr.type == TRACE_TYPE_PREFETCH_INSTR ||
        memref.instr.type == TRACE_TYPE_INSTR_NO_FETCH) {
        bool expect_encoding = TESTANY(OFFLINE_FILE_TYPE_ENCODINGS, shard->file_type_);
        std::unique_ptr<instr_autoclean_t> cur_instr_decoded = nullptr;
        if (expect_encoding) {
            cur_instr_decoded.reset(new instr_autoclean_t(GLOBAL_DCONTEXT));
            app_pc next_pc = decode_from_copy(
                GLOBAL_DCONTEXT, const_cast<app_pc>(memref.instr.encoding),
                reinterpret_cast<app_pc>(memref.instr.addr), cur_instr_decoded->data);
            if (next_pc == nullptr) {
                cur_instr_decoded.reset(nullptr);
            }
        }
        if (knob_verbose_ >= 3) {
            std::cerr << "::" << memref.data.pid << ":" << memref.data.tid << ":: "
                      << " @" << (void *)memref.instr.addr
                      << ((memref.instr.type == TRACE_TYPE_INSTR_NO_FETCH)
                              ? " non-fetched"
                              : "")
                      << " instr x" << memref.instr.size << "\n";
        }
#ifdef UNIX
        report_if_false(shard, shard->instrs_until_interrupt_ != 0,
                        "Interruption marker too late");
        if (shard->instrs_until_interrupt_ > 0)
            --shard->instrs_until_interrupt_;
#endif
        if (memref.instr.type == TRACE_TYPE_INSTR_DIRECT_CALL ||
            memref.instr.type == TRACE_TYPE_INSTR_INDIRECT_CALL) {
            shard->last_retaddr_ = memref.instr.addr + memref.instr.size;
        }
        // Invariant: offline traces guarantee that a branch target must immediately
        // follow the branch w/ no intervening thread switch.
        // If we did serial analyses only, we'd just track the previous instr in the
        // interleaved stream.  Here we look for headers indicating where an interleaved
        // stream *could* switch threads, so we're stricter than necessary.
        if (knob_offline_ && type_is_instr_branch(shard->prev_instr_.instr.type)) {
            report_if_false(
                shard,
                !shard->saw_timestamp_but_no_instr_ ||
                    // The invariant is relaxed for a signal.
                    // prev_xfer_marker_ is cleared on an instr, so if set to
                    // non-sentinel it means it is immediately prior, in
                    // between prev_instr_ and memref.
                    shard->prev_xfer_marker_.marker.marker_type ==
                        TRACE_MARKER_TYPE_KERNEL_EVENT ||
                    // Instruction-filtered are exempted.
                    TESTANY(OFFLINE_FILE_TYPE_FILTERED | OFFLINE_FILE_TYPE_IFILTERED,
                            shard->file_type_),
                "Branch target not immediately after branch");
        }
        // Invariant: non-explicit control flow (i.e., kernel-mediated) is indicated
        // by markers.
        const std::string non_explicit_flow_violation_msg =
            check_for_pc_discontinuity(shard, memref, cur_instr_decoded, expect_encoding);
        report_if_false(shard, non_explicit_flow_violation_msg.empty(),
                        non_explicit_flow_violation_msg);

#ifdef UNIX
        // Ensure signal handlers return to the interruption point.
        if (shard->prev_xfer_marker_.marker.marker_type ==
            TRACE_MARKER_TYPE_KERNEL_XFER) {
            // For the following checks, we use the values popped from the
            // signal_stack_ into last_signal_context_ at the last
            // TRACE_MARKER_TYPE_KERNEL_XFER marker.
            bool kernel_event_marker_equality =
                // Skip this check if we did not see the corresponding
                // kernel_event marker in the trace because the trace
                // started mid-signal.
                shard->last_signal_context_.xfer_int_pc == 0 ||
                // Regular check for equality with kernel event marker pc.
                memref.instr.addr == shard->last_signal_context_.xfer_int_pc ||
                // DR hands us a different address for sysenter than the
                // resumption point.
                shard->last_signal_context_.pre_signal_instr.instr.type ==
                    TRACE_TYPE_INSTR_SYSENTER;
            bool pre_signal_flow_continuity =
                // Skip pre-signal instr check if there was no such instr. May
                // happen for nested signals without any intervening instr, and
                // if the signal arrived before the first instr in the trace.
                shard->last_signal_context_.pre_signal_instr.instr.addr == 0 ||
                // Skip pre_signal_instr_ check for signals that caused an rseq
                // abort. In this case, control is transferred directly to the abort
                // handler, verified using last_signal_context_.xfer_int_pc above.
                shard->last_signal_context_.xfer_aborted_rseq ||
                // Pre-signal instr continued after signal.
                memref.instr.addr ==
                    shard->last_signal_context_.pre_signal_instr.instr.addr ||
                // Asynch will go to the subsequent instr.
                memref.instr.addr ==
                    shard->last_signal_context_.pre_signal_instr.instr.addr +
                        shard->last_signal_context_.pre_signal_instr.instr.size ||
                // Too hard to figure out branch targets.  We have the
                // last_signal_context_.xfer_int_pc though.
                // TODO i#5912: since we have the branch decoding now, we can handle
                // this case.
                type_is_instr_branch(
                    shard->last_signal_context_.pre_signal_instr.instr.type) ||
                shard->last_signal_context_.pre_signal_instr.instr.type ==
                    TRACE_TYPE_INSTR_SYSENTER;
            report_if_false(
                shard,
                (kernel_event_marker_equality && pre_signal_flow_continuity) ||
                    // Nested signal.  XXX: This only works for our annotated test
                    // signal_invariants where we know shard->app_handler_pc_.
                    memref.instr.addr == shard->app_handler_pc_ ||
                    // Marker for rseq abort handler.  Not as unique as a prefetch, but
                    // we need an instruction and not a data type.
                    memref.instr.type == TRACE_TYPE_INSTR_DIRECT_JUMP ||
                    // Instruction-filtered can easily skip the return point.
                    TESTANY(OFFLINE_FILE_TYPE_FILTERED | OFFLINE_FILE_TYPE_IFILTERED,
                            shard->file_type_),
                "Signal handler return point incorrect");
        }
        // last_instr_in_cur_context_ is recorded as the pre-signal instr when we see a
        // TRACE_MARKER_TYPE_KERNEL_EVENT marker. Note that we cannot perform this
        // book-keeping using prev_instr_ on the TRACE_MARKER_TYPE_KERNEL_EVENT marker.
        // E.g. if there was no instr between two nested signals, we do not want to
        // record any pre-signal instr for the second signal.
        shard->last_instr_in_cur_context_ = memref;
#endif
        shard->prev_instr_ = memref;
        shard->prev_instr_decoded_ = std::move(cur_instr_decoded);
        // Clear prev_xfer_marker_ on an instr (not a memref which could come between an
        // instr and a kernel-mediated far-away instr) to ensure it's *immediately*
        // prior (i#3937).
        shard->prev_xfer_marker_.marker.marker_type = TRACE_MARKER_TYPE_VERSION;
        shard->saw_timestamp_but_no_instr_ = false;
        // Clear window transitions on instrs.
        shard->window_transition_ = false;
    } else if (knob_verbose_ >= 3) {
        std::cerr << "::" << memref.data.pid << ":" << memref.data.tid << ":: "
                  << " type " << memref.instr.type << "\n";
    }
    if (memref.marker.type == TRACE_TYPE_MARKER &&
        memref.marker.marker_type == TRACE_MARKER_TYPE_TIMESTAMP) {
        shard->last_timestamp_ = memref.marker.marker_value;
        shard->saw_timestamp_but_no_instr_ = true;
        if (knob_verbose_ >= 3) {
            std::cerr << "::" << memref.data.pid << ":" << memref.data.tid << ":: "
                      << " timestamp " << memref.marker.marker_value << "\n";
        }
    }
    if (memref.marker.type == TRACE_TYPE_MARKER &&
        memref.marker.marker_type == TRACE_MARKER_TYPE_CPU_ID) {
        shard->sched_.emplace_back(shard->tid_, shard->last_timestamp_,
                                   memref.marker.marker_value, shard->instr_count_);
        shard->cpu2sched_[memref.marker.marker_value].emplace_back(
            shard->tid_, shard->last_timestamp_, memref.marker.marker_value,
            shard->instr_count_);
    }

#ifdef UNIX
    bool saw_rseq_abort = false;
#endif
    if (memref.marker.type == TRACE_TYPE_MARKER &&
        // Ignore timestamp, etc. markers which show up at signal delivery boundaries
        // b/c the tracer does a buffer flush there.
        (memref.marker.marker_type == TRACE_MARKER_TYPE_KERNEL_EVENT ||
         memref.marker.marker_type == TRACE_MARKER_TYPE_KERNEL_XFER)) {
        if (knob_verbose_ >= 3) {
            std::cerr << "::" << memref.data.pid << ":" << memref.data.tid << ":: "
                      << "marker type " << memref.marker.marker_type << " value 0x"
                      << std::hex << memref.marker.marker_value << std::dec << "\n";
        }
#ifdef UNIX
        report_if_false(shard, memref.marker.marker_value != 0,
                        "Kernel event marker value missing");
        if (memref.marker.marker_type == TRACE_MARKER_TYPE_KERNEL_XFER) {
            // We assume paired signal entry-exit (so no longjmp and no rseq
            // inside signal handlers).
            if (shard->signal_stack_.empty()) {
                // This can happen if tracing started in the middle of a signal.
                // Try to continue by skipping the checks.
                shard->last_signal_context_ = { 0, {}, false };
                // We have not seen any instr in the outermost scope that we just
                // discovered.
                shard->last_instr_in_cur_context_ = {};
            } else {
                // The pre_signal_instr for this signal may be {} in some cases:
                // - for nested signals without any intervening instr
                // - if there's a signal at the very beginning of the trace
                // In both these cases the empty instr implies that it should not
                // be used for the pre-signal instr check.
                shard->last_signal_context_ = shard->signal_stack_.top();
                shard->signal_stack_.pop();
                // In the case where there's no instr between two consecutive signals
                // (at the same nesting depth), the pre-signal instr for the second
                // signal should be same as the pre-signal instr for the first one.
                // Here we restore last_instr_in_cur_context_ to the last instr we
                // saw *in the same nesting depth* before the first signal.
                shard->last_instr_in_cur_context_ =
                    shard->last_signal_context_.pre_signal_instr;
            }
        }
        if (memref.marker.marker_type == TRACE_MARKER_TYPE_KERNEL_EVENT) {
            // If preceded by an RSEQ abort marker, this is not really a signal.
            if (shard->prev_entry_.marker.type == TRACE_TYPE_MARKER &&
                shard->prev_entry_.marker.marker_type == TRACE_MARKER_TYPE_RSEQ_ABORT) {
                saw_rseq_abort = true;
            } else {
                shard->signal_stack_.push({ memref.marker.marker_value,
                                            shard->last_instr_in_cur_context_,
                                            shard->saw_rseq_abort_ });
                // XXX: if last_instr_in_cur_context_ is {} currently, it means this is
                // either a signal that arrived before the first instr in the trace, or
                // it's a nested signal without any intervening instr after its
                // outer-scope signal. For the latter case, we can check if the
                // TRACE_MARKER_TYPE_KERNEL_EVENT marker value is equal for both signals.

                // We start with an empty memref_t to denote absence of any pre-signal
                // instr for any subsequent nested signals.
                shard->last_instr_in_cur_context_ = {};
            }
        }
#endif
        shard->prev_xfer_marker_ = memref;
        shard->last_xfer_marker_ = memref;
    }
    if (memref.marker.type == TRACE_TYPE_MARKER &&
        memref.marker.marker_type == TRACE_MARKER_TYPE_WINDOW_ID) {
        if (shard->last_window_ != memref.marker.marker_value)
            shard->window_transition_ = true;
        shard->last_window_ = memref.marker.marker_value;
    }

#ifdef UNIX
    if (saw_rseq_abort) {
        shard->saw_rseq_abort_ = true;
    }
    // If a signal caused an rseq abort, the signal's KERNEL_EVENT marker
    // will be preceded by an RSEQ_ABORT-KERNEL_EVENT marker pair. There may
    // be a buffer switch (denoted by the timestamp+cpu pair) between the
    // RSEQ_ABORT-KERNEL_EVENT pair and the signal's KERNEL_EVENT marker. We
    // want to ignore such an intervening timestamp+cpu marker pair when
    // checking whether a signal caused an RSEQ abort.
    else if (!(memref.marker.type == TRACE_TYPE_MARKER &&
               (memref.marker.marker_type == TRACE_MARKER_TYPE_TIMESTAMP ||
                memref.marker.marker_type == TRACE_MARKER_TYPE_CPU_ID))) {
        shard->saw_rseq_abort_ = false;
    }
    shard->prev_prev_entry_ = shard->prev_entry_;
#endif
    shard->prev_entry_ = memref;
    if (type_is_instr_branch(shard->prev_entry_.instr.type))
        shard->last_branch_ = shard->prev_entry_;
    return true;
}

bool
invariant_checker_t::process_memref(const memref_t &memref)
{
    per_shard_t *per_shard;
    const auto &lookup = shard_map_.find(memref.data.tid);
    if (lookup == shard_map_.end()) {
        auto per_shard_unique = std::unique_ptr<per_shard_t>(new per_shard_t);
        per_shard = per_shard_unique.get();
        per_shard->stream = serial_stream_;
        shard_map_[memref.data.tid] = std::move(per_shard_unique);
    } else
        per_shard = lookup->second.get();
    if (!parallel_shard_memref(reinterpret_cast<void *>(per_shard), memref)) {
        error_string_ = per_shard->error_;
        return false;
    }
    return true;
}

void
invariant_checker_t::check_schedule_data(per_shard_t *global)
{
    if (serial_schedule_file_ == nullptr && cpu_schedule_file_ == nullptr)
        return;
    // Check that the scheduling data in the files written by raw2trace match
    // the data in the trace.
    // Use a synthetic stream object to allow report_if_false to work normally.
    auto stream = std::unique_ptr<memtrace_stream_t>(
        new default_memtrace_stream_t(&global->ref_count_));
    global->stream = stream.get();
    std::vector<schedule_entry_t> serial;
    std::unordered_map<uint64_t, std::vector<schedule_entry_t>> cpu2sched;
    for (auto &shard_keyval : shard_map_) {
        serial.insert(serial.end(), shard_keyval.second->sched_.begin(),
                      shard_keyval.second->sched_.end());
        for (auto &keyval : shard_keyval.second->cpu2sched_) {
            auto &vec = cpu2sched[keyval.first];
            vec.insert(vec.end(), keyval.second.begin(), keyval.second.end());
        }
    }
    std::sort(serial.begin(), serial.end(),
              [](const schedule_entry_t &l, const schedule_entry_t &r) {
                  return l.timestamp < r.timestamp;
              });
    if (serial_schedule_file_ != nullptr) {
        schedule_entry_t next(0, 0, 0, 0);
        while (
            serial_schedule_file_->read(reinterpret_cast<char *>(&next), sizeof(next))) {
            report_if_false(global,
                            memcmp(&serial[static_cast<size_t>(global->ref_count_)],
                                   &next, sizeof(next)) == 0,
                            "Serial schedule entry does not match trace");
            ++global->ref_count_;
        }
        report_if_false(global, global->ref_count_ == serial.size(),
                        "Serial schedule entry count does not match trace");
    }
    if (cpu_schedule_file_ == nullptr)
        return;
    std::unordered_map<uint64_t, uint64_t> cpu2idx;
    for (auto &keyval : cpu2sched) {
        std::sort(keyval.second.begin(), keyval.second.end(),
                  [](const schedule_entry_t &l, const schedule_entry_t &r) {
                      return l.timestamp < r.timestamp;
                  });
        cpu2idx[keyval.first] = 0;
    }
    // The zipfile reader will form a continuous stream from all elements in the
    // archive.  We figure out which cpu each one is from on the fly.
    schedule_entry_t next(0, 0, 0, 0);
    while (cpu_schedule_file_->read(reinterpret_cast<char *>(&next), sizeof(next))) {
        global->ref_count_ = next.start_instruction;
        global->tid_ = next.thread;
        report_if_false(
            global,
            memcmp(&cpu2sched[next.cpu][static_cast<size_t>(cpu2idx[next.cpu])], &next,
                   sizeof(next)) == 0,
            "Cpu schedule entry does not match trace");
        ++cpu2idx[next.cpu];
    }
    for (auto &keyval : cpu2sched) {
        global->ref_count_ = 0;
        global->tid_ = keyval.first;
        report_if_false(global, cpu2idx[keyval.first] == keyval.second.size(),
                        "Cpu schedule entry count does not match trace");
    }
}

bool
invariant_checker_t::print_results()
{
    per_shard_t global;
    check_schedule_data(&global);
    std::cerr << "Trace invariant checks passed\n";
    return true;
}

std::string
invariant_checker_t::check_for_pc_discontinuity(
    per_shard_t *shard, const memref_t &memref,
    const std::unique_ptr<instr_autoclean_t> &cur_instr_decoded,
    const bool expect_encoding)
{
    std::string error_msg = "";
    bool have_cond_branch_target = false;
    addr_t cond_branch_target = 0;
    const addr_t prev_instr_trace_pc = shard->prev_instr_.instr.addr;
    if (prev_instr_trace_pc != 0 /*first*/ &&
        // We do not bother to support legacy traces without encodings.
        expect_encoding && type_is_instr_direct_branch(shard->prev_instr_.instr.type)) {
        if (shard->prev_instr_.instr.encoding_is_new)
            shard->branch_target_cache.erase(prev_instr_trace_pc);
        auto cached = shard->branch_target_cache.find(prev_instr_trace_pc);
        if (cached != shard->branch_target_cache.end()) {
            have_cond_branch_target = true;
            cond_branch_target = cached->second;
        } else {
            if (shard->prev_instr_decoded_ == nullptr ||
                !opnd_is_pc(instr_get_target(shard->prev_instr_decoded_->data))) {
                // Neither condition should happen but they could on an invalid
                // encoding from raw2trace or the reader so we report an
                // invariant rather than asserting.
                report_if_false(shard, false, "Branch target is not decodeable");
            } else {
                have_cond_branch_target = true;
                cond_branch_target = reinterpret_cast<addr_t>(
                    opnd_get_pc(instr_get_target(shard->prev_instr_decoded_->data)));
                shard->branch_target_cache[prev_instr_trace_pc] = cond_branch_target;
            }
        }
    }
    if (prev_instr_trace_pc != 0 /*first*/) {
        // Check for all valid transitions except taken branches. We consider taken
        // branches later so that we can provide a different message for those
        // invariant violations.
        const bool valid_nonbranch_flow =
            // Filtered.
            TESTANY(OFFLINE_FILE_TYPE_FILTERED | OFFLINE_FILE_TYPE_IFILTERED,
                    shard->file_type_) ||
            // Regular fall-through.
            (prev_instr_trace_pc + shard->prev_instr_.instr.size == memref.instr.addr) ||
            // String loop.
            (prev_instr_trace_pc == memref.instr.addr &&
             (memref.instr.type == TRACE_TYPE_INSTR_NO_FETCH ||
              // Online incorrectly marks the 1st string instr across a thread
              // switch as fetched.
              // TODO i#4915, #4948: Eliminate non-fetched and remove the
              // underlying instrs altogether, which would fix this for us.
              (!knob_offline_ && shard->saw_timestamp_but_no_instr_))) ||
            // Kernel-mediated, but we can't tell if we had a thread swap.
            (shard->prev_xfer_marker_.instr.tid != 0 &&
             (shard->prev_xfer_marker_.marker.marker_type ==
                  TRACE_MARKER_TYPE_KERNEL_EVENT ||
              shard->prev_xfer_marker_.marker.marker_type ==
                  TRACE_MARKER_TYPE_KERNEL_XFER ||
              shard->prev_xfer_marker_.marker.marker_type ==
                  TRACE_MARKER_TYPE_RSEQ_ABORT)) ||
            // We expect a gap on a window transition.
            shard->window_transition_ ||
            shard->prev_instr_.instr.type == TRACE_TYPE_INSTR_SYSENTER;

        if (!valid_nonbranch_flow) {
            // Check if the type is a branch instruction and there is a branch target
            // mismatch.
            if (type_is_instr_branch(shard->prev_instr_.instr.type)) {
                const bool valid_branch_flow =
                    // Indirect branches we cannot check.
                    !type_is_instr_direct_branch(shard->prev_instr_.instr.type) ||
                    // Conditional fall-through hits the regular case above.
                    !have_cond_branch_target || memref.instr.addr == cond_branch_target;

                if (!valid_branch_flow) {
                    error_msg = "Direct branch does not go to the correct target";
                }
            } else if (cur_instr_decoded != nullptr &&
                       shard->prev_instr_decoded_ != nullptr &&
                       instr_is_syscall(cur_instr_decoded->data) &&
                       memref.instr.addr == prev_instr_trace_pc &&
                       instr_is_syscall(shard->prev_instr_decoded_->data)) {
                error_msg = "Duplicate syscall instrs with the same PC";
            } else if (shard->prev_instr_decoded_ != nullptr &&
                       instr_writes_memory(shard->prev_instr_decoded_->data) &&
                       type_is_instr_conditional_branch(shard->last_branch_.instr.type)) {
                // This sequence happens when an rseq side exit occurs which
                // results in missing instruction in the basic block.
                error_msg = "PC discontinuity due to rseq side exit";
            } else {
                error_msg = "Non-explicit control flow has no marker";
            }
        }
    }

    return error_msg;
}
