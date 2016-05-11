#ifndef SRC_INSPECTOR_AGENT_H_
#define SRC_INSPECTOR_AGENT_H_

#if !HAVE_INSPECTOR
#   error("This header can only be used when inspector is enabled")
#endif

#include "inspector_socket.h"
#include "uv.h"
#include "v8.h"
#include "util.h"

namespace blink {
class V8Inspector;
namespace protocol {
  class String16;
}
}

// Forward declaration to break recursive dependency chain with src/env.h.
namespace node {
class Environment;
}  // namespace node

namespace node {
namespace inspector {

class ChannelImpl;

class MessageFromFrontend {
public:
  MessageFromFrontend(char* message, size_t length) : message_(message),
                                                          length_(length) {
  }
  ~MessageFromFrontend() {
    free(message_);
    message_ = nullptr;
  }
  const char* message() { return message_; }
  size_t length() { return length_; }

  ListNode<MessageFromFrontend> listNode;
private:
  char* message_;
  const size_t length_;
};

class Agent {
 public:
  explicit Agent(node::Environment* env);
  ~Agent();

  // Start the inspector agent thread
  bool Start(v8::Platform* platform, int port, bool wait);
  // Stop the inspector agent
  void Stop();

  void PostMessages();

  bool connected() {  return connected_; }

 protected:
  inline node::Environment* parent_env() {  return parent_env_; }
  void InitAdaptor(Environment* env);

  // Worker body
  void WorkerRun();

  static void ThreadCb(Agent* agent);
  static void ParentSignalCb(uv_async_t* signal);

  uv_sem_t start_sem_;
  uv_mutex_t queue_lock_;
  ListHead<MessageFromFrontend, &MessageFromFrontend::listNode> message_queue_;

  int port_;
  bool wait_;
  bool connected_;

  uv_thread_t thread_;
  node::Environment* parent_env_;
  uv_loop_t child_loop_;

  uv_tcp_t server_;
  uv_async_t dataWritten_;
  // Currently it is simply the last inspector connection. Later we may consider
  // some sort of policy - e.g. closing a previous connection or supporting
  // multiple connections.
  inspector_socket_t* client_socket_;
  blink::V8Inspector* inspector_;
  v8::Platform* platform_;

  static bool OnInspectorHandshake(inspector_socket_t* socket,
                                   enum inspector_handshake_event state,
                                   const char* path);
  static void OnSocketConnection(uv_stream_t* server, int status);
  static void OnRemoteData(uv_stream_t* stream, ssize_t read,
      const uv_buf_t* b);
  static void WriteCb(uv_async_t* async);

  bool RespondToGet(inspector_socket_t* socket, const char* path);
  bool AcceptsConnection(inspector_socket_t* socket, const char* path);
  void OnInspectorConnection(inspector_socket_t* socket);
  void write(const blink::protocol::String16& message);

  friend class AsyncWriteRequest;
  friend class ChannelImpl;
  friend class SetConnectedTask;
};

}  // namespace inspector
}  // namespace node

#endif  // SRC_INSPECTOR_AGENT_H_
