/*
 *
 * Copyright 2015 gRPC authors.
 *
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
 *
 */

#include <iostream>
#include <memory>
#include <string>
#include <thread>

#include "unistd.h"

#include <grpcpp/grpcpp.h>

#ifdef BAZEL_BUILD
#include "examples/protos/helloworld.grpc.pb.h"
#else
#include "helloworld.grpc.pb.h"
#endif

using grpc::Channel;
using grpc::ClientContext;
using grpc::ClientReader;
using grpc::Status;
using namespace helloworld;

class GreeterClient {
 public:
  GreeterClient(std::shared_ptr<Channel> channel)
      : stub_(Greeter::NewStub(channel)) {}

  ~GreeterClient()
  {
    StopMessageThread();
  }

  void StopMessageThread()
  {
    // メッセージ受信スレッドの終了
    if (m_message_thread.joinable()) {
      m_message_thread.join();
    }
  }

  bool CreateUser(const std::string& user_name) {
    CreateUserRequest request;
    request.set_user_name(user_name);

    CreateUserReply reply;
    ClientContext context;
    Status status = stub_->CreateUser(&context, request, &reply);

    if (status.ok()) {
      m_user_id = reply.user_id();
      std::cout << "user_id:" << m_user_id << std::endl; // debug
      m_user_name = user_name;
      return true;
    } else {
      std::cout << status.error_code() << ": " << status.error_message() << std::endl;
      return false;
    }
  }

  int GetRoomID(const std::string& room_name) {
    GetRoomIDRequest request;
    request.set_room_name(room_name);

    GetRoomIDReply reply;
    ClientContext context;
    Status status = stub_->GetRoomID(&context, request, &reply);

    if (status.ok()) {
      int room_id = reply.room_id();
      std::cout << "room_id:" << room_id << std::endl; // debug
      return room_id;
    } else {
      std::cout << status.error_code() << ": " << status.error_message() << std::endl;
      return -1;
    }
  }

  bool SendMessage(const std::string& message) {
    ChatMessage request;
    request.set_room_id(m_room_id);
    request.set_user_name(m_user_name);
    request.set_message(message);

    google::protobuf::Empty reply;
    ClientContext context;
    Status status = stub_->SendMessage(&context, request, &reply);

    if (status.ok()) {
      return true;
    } else {
      std::cout << status.error_code() << ": " << status.error_message() << std::endl;
      return false;
    }
  }

  void JoinRoom() {
    JoinRoomRequest request;
    request.set_user_id(m_user_id);
    request.set_room_id(m_room_id);
    ClientContext context;
    std::unique_ptr<ClientReader<ChatMessage> > reader(
      stub_->JoinRoom(&context, request)
    );
    ChatMessage message;
    while (reader->Read(&message)) {
      // メッセージ受信部
      std::cout << "[" << message.user_name() << "] " << message.message() << std::endl;
    }
    Status status = reader->Finish();
    if (status.ok()) {
      // leaveした場合に受信が終了する
      std::cout << "Leave Successed." << std::endl;
    } else {
      std::cout << "stream rpc abort." << std::endl;
    }
  }

  bool LeaveRoom() {
    LeaveRoomRequest request;
    request.set_user_id(m_user_id);
    request.set_room_id(m_room_id);

    google::protobuf::Empty reply;
    ClientContext context;
    Status status = stub_->LeaveRoom(&context, request, &reply);

    if (status.ok()) {
      StopMessageThread();
      m_room_id = 0;
      return true;
    } else {
      std::cout << status.error_code() << ": " << status.error_message()
                << std::endl;
      return false;
    }
  }

  bool IsChatCommand(const std::string& str)
  {
    if (str.empty()) return false;
    return (str[0] == '/');
  }

  bool SendChatCommand(const std::string& command)
  {
    if (command == "/room") {
      std::cout << "room list request" << std::endl;
      return ListRooms();
    }
    if (command.find("/join ") != std::string::npos) {
      if (m_room_id != 0) {
        // とりあえず同時に入れるルームは1個だけとする
        std::cout << "already joined any room!" << std::endl;
        return false;
      }
      // TODO:trimした方が良い
      std::string room_name = command.substr(6);
      m_room_id = this->GetRoomID(room_name);
      m_message_thread = std::thread(&GreeterClient::JoinRoom, this); // JoinRoomはルームから離脱するまでblockingするので別スレッドで行う
      return true;
    }
    if (command == "/leave") {
      std::cout << "leave request" << std::endl;
      return LeaveRoom();
    }
    std::cout << "unknown command" << std::endl;
    return false;
  }

  bool ListRooms()
  {
    google::protobuf::Empty request;
    ListRoomsReply reply;
    ClientContext context;
    Status status = stub_->ListRooms(&context, request, &reply);

    if (status.ok()) {
      int size = reply.rooms_size();
      for (int i = 0; i < size; ++i) {
        const auto& room = reply.rooms(i);
        std::cout << room.room_name() << std::endl;
      }
      return true;
    } else {
      std::cout << status.error_code() << ": " << status.error_message()
                << std::endl;
      return false;
    }
  }

 private:
  std::unique_ptr<Greeter::Stub> stub_;
  std::thread m_message_thread;
  std::string m_user_name;
  int32_t m_user_id = 0;
  int32_t m_room_id = 0;
};

int main(int argc, char** argv) {
  // Instantiate the client. It requires a channel, out of which the actual RPCs
  // are created. This channel models a connection to an endpoint specified by
  // the argument "--target=" which is the only expected argument.
  // We indicate that the channel isn't authenticated (use of
  // InsecureChannelCredentials()).
  std::string target_str;
  std::string arg_str("--target");
  if (argc > 1) {
    std::string arg_val = argv[1];
    size_t start_pos = arg_val.find(arg_str);
    if (start_pos != std::string::npos) {
      start_pos += arg_str.size();
      if (arg_val[start_pos] == '=') {
        target_str = arg_val.substr(start_pos + 1);
      } else {
        std::cout << "The only correct argument syntax is --target=" << std::endl;
        return 0;
      }
    } else {
      std::cout << "The only acceptable argument is --target=" << std::endl;
      return 0;
    }
  } else {
    target_str = "localhost:50051";
  }
  GreeterClient greeter(grpc::CreateChannel(
      target_str, grpc::InsecureChannelCredentials()));

  std::string user_name;
  std::cout << "type user name. " << std::endl;
  std::getline(std::cin, user_name);

  greeter.CreateUser( user_name );
  while (!std::cin.eof()) // Ctrl + Dで終了
  {
    std::string message;
    std::cout << "type message. " << std::endl;
    std::getline(std::cin, message);
    if (!message.empty()) {
      if (greeter.IsChatCommand(message)) {
        greeter.SendChatCommand(message);
      } else {
        greeter.SendMessage(message);
      }
    } else {
      break;
    }
  }
  // 未離脱ならここで離脱する
  greeter.LeaveRoom();
  return 0;
}
