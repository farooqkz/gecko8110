// Copyright (c) 2006-2008 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/common/ipc_message.h"

#include "base/logging.h"
#include "build/build_config.h"

#if defined(OS_POSIX)
#include "chrome/common/file_descriptor_set_posix.h"
#endif
#ifdef MOZ_TASK_TRACER
#include "GeckoTaskTracerImpl.h"
#endif

#include "mozilla/Move.h"

#ifdef MOZ_TASK_TRACER
using namespace mozilla::tasktracer;
#endif

namespace IPC {

//------------------------------------------------------------------------------

Message::~Message() {
  MOZ_COUNT_DTOR(IPC::Message);
}

Message::Message()
    : Pickle(sizeof(Header)) {
  MOZ_COUNT_CTOR(IPC::Message);
  header()->routing = header()->type = header()->flags = 0;
#if defined(OS_POSIX)
  header()->num_fds = 0;
#endif
#ifdef MOZ_TASK_TRACER
  GetCurTraceInfo(&header()->source_event_id,
                  &header()->parent_task_id,
                  &header()->source_event_type);
#endif
  InitLoggingVariables();
}

Message::Message(int32_t routing_id, msgid_t type, PriorityValue priority,
                 MessageCompression compression, const char* const aName)
    : Pickle(sizeof(Header)) {
  MOZ_COUNT_CTOR(IPC::Message);
  header()->routing = routing_id;
  header()->type = type;
  header()->flags = priority;
  if (compression == COMPRESSION_ENABLED)
    header()->flags |= COMPRESS_BIT;
  else if (compression == COMPRESSION_ALL)
    header()->flags |= COMPRESSALL_BIT;
#if defined(OS_POSIX)
  header()->num_fds = 0;
#endif
  header()->interrupt_remote_stack_depth_guess = static_cast<uint32_t>(-1);
  header()->interrupt_local_stack_depth = static_cast<uint32_t>(-1);
  header()->seqno = 0;
#if defined(OS_MACOSX)
  header()->cookie = 0;
#endif
#ifdef MOZ_TASK_TRACER
  GetCurTraceInfo(&header()->source_event_id,
                  &header()->parent_task_id,
                  &header()->source_event_type);
#endif
  InitLoggingVariables(aName);
}

Message::Message(const char* data, int data_len, Ownership ownership)
  : Pickle(data, data_len, ownership)
{
  MOZ_COUNT_CTOR(IPC::Message);
  InitLoggingVariables();
}

Message::Message(const Message& other) : Pickle(other) {
  MOZ_COUNT_CTOR(IPC::Message);
  InitLoggingVariables(other.name_);
#if defined(OS_POSIX)
  file_descriptor_set_ = other.file_descriptor_set_;
#endif
#ifdef MOZ_TASK_TRACER
  header()->source_event_id = other.header()->source_event_id;
  header()->parent_task_id = other.header()->parent_task_id;
  header()->source_event_type = other.header()->source_event_type;
#endif
}

Message::Message(Message&& other) : Pickle(mozilla::Move(other)) {
  MOZ_COUNT_CTOR(IPC::Message);
  InitLoggingVariables(other.name_);
#if defined(OS_POSIX)
  file_descriptor_set_ = other.file_descriptor_set_.forget();
#endif
}

void Message::InitLoggingVariables(const char* const aName) {
  name_ = aName;
}

Message& Message::operator=(const Message& other) {
  *static_cast<Pickle*>(this) = other;
  InitLoggingVariables(other.name_);
#if defined(OS_POSIX)
  file_descriptor_set_ = other.file_descriptor_set_;
#endif
#ifdef MOZ_TASK_TRACER
  header()->source_event_id = other.header()->source_event_id;
  header()->parent_task_id = other.header()->parent_task_id;
  header()->source_event_type = other.header()->source_event_type;
#endif
  return *this;
}

Message& Message::operator=(Message&& other) {
  *static_cast<Pickle*>(this) = mozilla::Move(other);
  InitLoggingVariables(other.name_);
#if defined(OS_POSIX)
  file_descriptor_set_.swap(other.file_descriptor_set_);
#endif
  return *this;
}


#if defined(OS_POSIX)
bool Message::WriteFileDescriptor(const base::FileDescriptor& descriptor) {
  // We write the index of the descriptor so that we don't have to
  // keep the current descriptor as extra decoding state when deserialising.
  // Also, we rely on each file descriptor being accompanied by sizeof(int)
  // bytes of data in the message. See the comment for input_cmsg_buf_.
  WriteInt(file_descriptor_set()->size());
  if (descriptor.auto_close) {
    return file_descriptor_set()->AddAndAutoClose(descriptor.fd);
  } else {
    return file_descriptor_set()->Add(descriptor.fd);
  }
}

bool Message::ReadFileDescriptor(void** iter,
                                base::FileDescriptor* descriptor) const {
  int descriptor_index;
  if (!ReadInt(iter, &descriptor_index))
    return false;

  FileDescriptorSet* file_descriptor_set = file_descriptor_set_.get();
  if (!file_descriptor_set)
    return false;

  descriptor->fd = file_descriptor_set->GetDescriptorAt(descriptor_index);
  descriptor->auto_close = false;

  return descriptor->fd >= 0;
}

void Message::EnsureFileDescriptorSet() {
  if (file_descriptor_set_.get() == NULL)
    file_descriptor_set_ = new FileDescriptorSet;
}

uint32_t Message::num_fds() const {
  return file_descriptor_set() ? file_descriptor_set()->size() : 0;
}

#endif

#ifdef MOZ_TASK_TRACER
void *MessageTask() {
  return reinterpret_cast<void*>(&MessageTask);
}

void
Message::TaskTracerDispatch() {
  header()->task_id = GenNewUniqueTaskId();
  uintptr_t* vtab = reinterpret_cast<uintptr_t*>(&MessageTask);
  LogVirtualTablePtr(header()->task_id,
                     header()->source_event_id,
                     vtab);
  LogDispatch(header()->task_id,
              header()->parent_task_id,
              header()->source_event_id,
              header()->source_event_type);
}

Message::AutoTaskTracerRun::AutoTaskTracerRun(Message& aMsg)
  : mMsg(aMsg)
  , mTaskId(mMsg.header()->task_id)
  , mSourceEventId(mMsg.header()->source_event_id) {
  LogBegin(mMsg.header()->task_id,
           mMsg.header()->source_event_id);
  SetCurTraceInfo(mMsg.header()->source_event_id,
                  mMsg.header()->task_id,
                  mMsg.header()->source_event_type);
}

Message::AutoTaskTracerRun::~AutoTaskTracerRun() {
  AddLabel("IPC Message %s", mMsg.name());
  LogEnd(mTaskId, mSourceEventId);
}
#endif

}  // namespace IPC
