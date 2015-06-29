/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/vfs/entry.h"

#include "xenia/base/filesystem.h"
#include "xenia/base/string.h"
#include "xenia/vfs/device.h"

namespace xe {
namespace vfs {

Entry::Entry(Device* device, const std::string& path)
    : device_(device),
      path_(path),
      attributes_(0),
      size_(0),
      allocation_size_(0),
      create_timestamp_(0),
      access_timestamp_(0),
      write_timestamp_(0) {
  assert_not_null(device);
  absolute_path_ = xe::join_paths(device->mount_path(), path);
  name_ = xe::find_name_from_path(path);
}

Entry::~Entry() = default;

void Entry::Dump(xe::StringBuffer* string_buffer, int indent) {
  for (int i = 0; i < indent; ++i) {
    string_buffer->Append(' ');
  }
  string_buffer->Append(name());
  string_buffer->Append('\n');
  for (auto& child : children_) {
    child->Dump(string_buffer, indent + 2);
  }
}

bool Entry::is_read_only() const { return device_->is_read_only(); }

Entry* Entry::GetChild(std::string name) {
  // TODO(benvanik): a faster search
  for (auto& child : children_) {
    if (strcasecmp(child->name().c_str(), name.c_str()) == 0) {
      return child.get();
    }
  }
  return nullptr;
}

Entry* Entry::IterateChildren(const xe::filesystem::WildcardEngine& engine,
                              size_t* current_index) {
  while (*current_index < children_.size()) {
    auto& child = children_[*current_index];
    *current_index = *current_index + 1;
    if (engine.Match(child->name())) {
      return child.get();
    }
  }
  return nullptr;
}

}  // namespace vfs
}  // namespace xe
