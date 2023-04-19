// Copyright (c) 2023 dingodb.com, Inc. All Rights Reserved
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

#include "integer_schema.h"

namespace dingodb {

int DingoSchema<optional<int32_t>>::GetDataLength() {
  return 4;
}
int DingoSchema<optional<int32_t>>::GetWithNullTagLength() {
  return 5;
}
void DingoSchema<optional<int32_t>>::InternalEncodeNull(Buf* buf) {
  buf->Write(0);
  buf->Write(0);
  buf->Write(0);
  buf->Write(0);
}
void DingoSchema<optional<int32_t>>::InternalEncodeKey(Buf*  buf, int32_t data) {
  uint32_t* i = (uint32_t*)&data;
  buf->Write(*i >> 24 ^ 0x80);
  buf->Write(*i >> 16);
  buf->Write(*i >> 8);
  buf->Write(*i);
}
void DingoSchema<optional<int32_t>>::InternalEncodeValue(Buf*  buf, int32_t data) {
  uint32_t* i = (uint32_t*)&data;
  buf->Write(*i >> 24);
  buf->Write(*i >> 16);
  buf->Write(*i >> 8);
  buf->Write(*i);
}

BaseSchema::Type DingoSchema<optional<int32_t>>::GetType() {
  return kInteger;
}
void DingoSchema<optional<int32_t>>::SetIndex(int index) {
  this->index_ = index;
}
int DingoSchema<optional<int32_t>>::GetIndex() {
  return this->index_;
}
void DingoSchema<optional<int32_t>>::SetIsKey(bool key) {
  this->key_ = key;
}
bool DingoSchema<optional<int32_t>>::IsKey() {
  return this->key_;
}
int DingoSchema<optional<int32_t>>::GetLength() {
  if (this->allow_null_) {
    return GetWithNullTagLength();
  }
  return GetDataLength();
}
void DingoSchema<optional<int32_t>>::SetAllowNull(bool allow_null) {
  this->allow_null_ = allow_null;
}
bool DingoSchema<optional<int32_t>>::AllowNull() {
  return this->allow_null_;
}
void DingoSchema<optional<int32_t>>::EncodeKey(Buf* buf, optional<int32_t> data) {
  if (this->allow_null_) {
    buf->EnsureRemainder(GetWithNullTagLength());
    if (data.has_value()) {
      buf->Write(k_not_null);
      InternalEncodeKey(buf, data.value());
    } else {
      buf->Write(k_null);
      InternalEncodeNull(buf);
    }
  } else {
    if (data.has_value()) {
      buf->EnsureRemainder(GetDataLength());
      InternalEncodeKey(buf, data.value());
    } else {
      // WRONG EMPTY DATA
    }
  }
}
optional<int32_t> DingoSchema<optional<int32_t>>::DecodeKey(Buf* buf) {
  if (this->allow_null_) {
    if (buf->Read() == this->k_null) {
      buf->Skip(GetDataLength());
      return nullopt;
    }
  }
  uint32_t r = ((buf->Read() & 0xFF ^ 0x80) << 24) |
               ((buf->Read() & 0xFF) << 16) | ((buf->Read() & 0xFF) << 8) |
               (buf->Read() & 0xFF);
  return r;
}
void DingoSchema<optional<int32_t>>::SkipKey(Buf* buf) {
  buf->Skip(GetLength());
}
void DingoSchema<optional<int32_t>>::EncodeValue(Buf* buf, optional<int32_t> data) {
  if (this->allow_null_) {
    buf->EnsureRemainder(GetWithNullTagLength());
    if (data.has_value()) {
      buf->Write(k_not_null);
      InternalEncodeValue(buf, data.value());
    } else {
      buf->Write(k_null);
      InternalEncodeNull(buf);
    }
  } else {
    if (data.has_value()) {
      buf->EnsureRemainder(GetDataLength());
      InternalEncodeValue(buf, data.value());
    } else {
      // WRONG EMPTY DATA
    }
  }
}
optional<int32_t> DingoSchema<optional<int32_t>>::DecodeValue(Buf* buf) {
  if (this->allow_null_) {
    if (buf->Read() == this->k_null) {
      buf->Skip(GetDataLength());
      return nullopt;
    }
  }
  uint32_t r = ((buf->Read() & 0xFF) << 24) | ((buf->Read() & 0xFF) << 16) |
               ((buf->Read() & 0xFF) << 8) | (buf->Read() & 0xFF);
  return r;
}
void DingoSchema<optional<int32_t>>::SkipValue(Buf* buf) {
  buf->Skip(GetLength());
}

}  // namespace dingodb