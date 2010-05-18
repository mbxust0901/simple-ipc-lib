#pragma once

#include <vector>
#include "ipc_wire_types.h"

///////////////////////////////////////////////////////////////////////////////////////////////////
// The channel class is a controler/coordinator between the different IPC actors:
// 1- the transport (like a pipe)
// 2- the message encoder
// 3- the message decoder
// 4- the message
//
// The unit of communication of a channel is the message. The channel does not differenciate
// sender or receiver and assumes that the transport is bi-directional.
// 
// For outgoing messages, a message is just defined as an array of WireType pointers along with
// a message id. The encoder and decoder are loosely coupled with the message and it is the job
// of the channel to interface them.
//
// Sending Requirements
//  Encoder should implement:
//    bool Open(int n_args)
//    bool Close()
//    void SetMsgId(int msg_id)
//    bool OnWord(void* bits, int tag)
//    bool OnString8(const std::string& s, int tag)
//    bool OnString16(const std::wstring& s, int tag)
//    bool OnUnixFd(int fd, int tag)
//    bool OnWinHandle(void* handle, int tag)
//    const void* GetBuffer(size_t* sz)
//
// Receiving Requirements
//  Decoder<Handler> should implement:
//    bool OnData(const char* buff, size_t sz)
//    bool Success()
//  Decoder<Handler> should call:
//    bool Handler::OnMessageStart(int id, int n_args)
//    bool Handler::OnWord(const void* bits, int type_id)
//    bool Handler::OnString8(std::string& str, int type_id) 
//    bool Handler::OnString16(std::wstring& str, int type_id)
//

namespace ipc {

template <class TransportT, class EncoderT, template <class> class DecoderT>
class Channel {
 public:
  static const size_t kMaxNumArgs = 8;
  Channel(TransportT* transport) : transport_(transport) {}

  size_t Send(int msg_id, const WireType* const args[], int n_args)  {
    EncoderT encoder;
    encoder.Open(n_args);
    for (int ix= 0; ix != n_args; ++ix) {
      AddMsgElement(&encoder, *args[ix]);
    }

    encoder.SetMsgId(msg_id);
    if (!encoder.Close())
      return -1;

    size_t size;
    const void* buf = encoder.GetBuffer(&size);
    if (!buf)
      return -2;
    return transport_->Send(buf, size);
  }

  template <class DispatchT>
  size_t Receive(DispatchT* top_dispatch) {
    RxHandler handler;
    DecoderT<RxHandler> decoder(&handler);
    size_t received = 0;
    const char* buf = NULL;
    do {
      buf = transport_->Receive(&received);
    } while (decoder.OnData(buf, received));

    if(!decoder.Success())
      return -1;

    size_t np = handler.GetArgCount();
    if (np > kMaxNumArgs)
      return -2;

    const WireType* args[kMaxNumArgs];
    for (size_t ix = 0; ix != np; ++ix) {
      args[ix] = &handler.GetArg(ix);
    }

    DispatchT* dispatch = top_dispatch->MsgHandler(handler.MsgId());
    return dispatch ? dispatch->OnMsgIn(handler.MsgId(), this, args, np) : -3;
  }

  class RxHandler {
   public:
    RxHandler() : msg_id_(-1) {} 

    bool OnMessageStart(int id, int n_args) {
      msg_id_ = id;
      list_.reserve(n_args);
      return true;
    }

    bool OnWord(const void* bits, int type_id) {
      switch (type_id) {
        case ipc::TYPE_INT32:
          list_.push_back(WireType(int(*reinterpret_cast<const int*>(bits))));
          break;
        case ipc::TYPE_UINT32:
          list_.push_back(WireType(unsigned int(*reinterpret_cast<const unsigned int*>(bits))));
          break;
        case ipc::TYPE_CHAR8:
          list_.push_back(WireType(char(*reinterpret_cast<const char*>(bits))));
          break;
        case ipc::TYPE_CHAR16:
          list_.push_back(WireType(wchar_t(*reinterpret_cast<const wchar_t*>(bits))));
          break;
        case ipc::TYPE_NULLSTRING8:
          list_.push_back(WireType(static_cast<char*>(NULL)));
          break;
        case ipc::TYPE_NULLSTRING16:
          list_.push_back(WireType(static_cast<wchar_t*>(NULL)));
          break;
        default: 
          return false;
      }
      return true;
    }

    bool OnString8(std::string& str, int type_id) {
      switch (type_id) {
        case ipc::TYPE_STRING8:
          list_.push_back(WireType(str.c_str()));
          break;
        case ipc::TYPE_BARRAY:
          list_.push_back(WireType(ByteArray(str.size(), &str[0])));
          break;
        default: 
          return false;
      }
      return true;
    }

    bool OnString16(std::wstring& str, int type_id) {
      switch (type_id) {
        case ipc::TYPE_STRING16:
          list_.push_back(WireType(str.c_str()));
          break;
        default: 
          return false;
      }
      return true;
    }

    int MsgId() const { return msg_id_; }
    
    const WireType& GetArg(size_t ix) {
      return list_[ix];
    }

    size_t GetArgCount() const { return list_.size(); }

  private:
    typedef std::vector<WireType> RxList;
    RxList list_;
    int msg_id_;
  };

private:
  // Uses |EncoderT| to encode one message element in the outgoing buffer.
  bool AddMsgElement(EncoderT* encoder, const WireType& wtype) {
    switch (wtype.Id()) {
      case ipc::TYPE_NONE:
        return false;

      case ipc::TYPE_INT32:
      case ipc::TYPE_UINT32:
      case ipc::TYPE_CHAR8:
      case ipc::TYPE_CHAR16:
        return encoder->OnWord(wtype.GetAsBits(), wtype.Id());

      case ipc::TYPE_STRING8:
      case ipc::TYPE_BARRAY: {
          std::string ctemp;
          wtype.GetString8(&ctemp);
          return encoder->OnString8(ctemp, wtype.Id());
        }

      case ipc::TYPE_STRING16: {
        std::wstring wtemp;
          wtype.GetString16(&wtemp);
          return encoder->OnString16(wtemp, wtype.Id());
        }

      case ipc::TYPE_NULLSTRING8:
      case ipc::TYPE_NULLSTRING16:
        return encoder->OnWord(wtype.GetAsBits(), wtype.Id());

      case ipc::TYPE_UNIX_FD:
        return encoder->OnUnixFd(wtype.GetUnixFD(), wtype.Id());

      case ipc::TYPE_WIN_HANDLE:
        return encoder->OnWinHandle(wtype.GetWinHandle(), wtype.Id());

      default:
        return false;
    }
  }

  TransportT* transport_;
};

}  // namespace ipc.