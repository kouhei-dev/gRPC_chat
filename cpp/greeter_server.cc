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
#include <vector>
#include <mutex>

#include <grpcpp/grpcpp.h>
#include <grpcpp/health_check_service_interface.h>
#include <grpcpp/ext/proto_server_reflection_plugin.h>

#ifdef BAZEL_BUILD
#include "examples/protos/helloworld.grpc.pb.h"
#else
#include "helloworld.grpc.pb.h"
#endif

#include "unistd.h"

using grpc::Server;
using grpc::ServerBuilder;
using grpc::ServerContext;
using grpc::ServerWriter;
using grpc::Status;
using grpc::StatusCode;
using namespace google::protobuf::util::error;
using namespace helloworld;

class RoomMgr;

class User {
public:
  User() = default;
  User(int id, const std::string& name)
    : m_user_id(id)
    , m_user_name(name)
    , m_writer(nullptr) // JoinRoom時にset
    , m_receivable(false)
  {}

  // Sayのような関数があってもよいかも

  void RecvChat(const ChatMessage& chat) {
    if (!is_receivable()) return;
    if (!m_writer) return;
    // TODO:lockが要るかも
    m_writer->Write(chat);
  }
  void set_receivable(bool flag) { m_receivable = flag; }

  int user_id() const { return m_user_id; }
  const std::string& user_name() const { return m_user_name; }
  void set_writer(ServerWriter<ChatMessage>* writer) { m_writer = writer; }
  bool is_receivable() const { return m_receivable; }
private:
  int m_user_id = 0;
  std::string m_user_name;
  ServerWriter<ChatMessage>* m_writer = nullptr;
  bool m_receivable = false;
};

using UserPtr = std::shared_ptr<User>;

class UserMgr {
public:
  static UserMgr& instance() {
    static UserMgr instance;
    return instance;
  }

  int CreateUser(const std::string& user_name) {
    std::lock_guard<std::mutex> lock( m_users_mutex );
    static int generated_user_id = 0;
    int user_id = ++generated_user_id;
    UserPtr user = std::make_shared<User>(user_id, user_name);
    m_users.emplace( user_id, user );
    return user_id;
  }

  UserPtr FindUser(int user_id) {
    std::lock_guard<std::mutex> lock( m_users_mutex );
    auto it = m_users.find(user_id);
    if (it == m_users.end()) return nullptr;
    return it->second;
  }

  // TODO:ユーザ削除

private:
  UserMgr() = default;
  std::map<int, UserPtr> m_users;
  std::mutex m_users_mutex;
};

class Room {
public:
  Room() = default;
  Room(int id, const std::string& name)
    : m_room_id(id)
    , m_room_name(name)
    , m_users()
  {}
  void AddUser(const UserPtr& user) {
    std::lock_guard<std::mutex> guard( m_users_mutex );
    m_users.emplace( user->user_id(), user );
  }

  void RecvChat(const ChatMessage& chat) {
    std::lock_guard<std::mutex> guard( m_users_mutex );
    // 全員にブロードキャスト
    for (auto& pair : m_users) {
      UserPtr& user = pair.second;
      user->RecvChat(chat); // TODO:通信エラーが発生したユーザーはルームから離脱させてもよいかもしれない
    }
  }

  void RemoveUser(int user_id) {
    std::lock_guard<std::mutex> guard( m_users_mutex );
    m_users.erase(user_id);
    // TODO:0人になったルームの削除
  }

  bool Empty() {
    std::lock_guard<std::mutex> guard( m_users_mutex );
    return m_users.empty();
  }

  int room_id() const { return m_room_id; }
  const std::string& room_name() const { return m_room_name; }

private:
  int m_room_id = 0;
  std::string m_room_name;
  std::map<int, UserPtr> m_users;
  std::mutex m_users_mutex;
};

using RoomPtr = std::shared_ptr<Room>;

class RoomMgr {
public:
  static RoomMgr& instance() {
    static RoomMgr instance;
    return instance;
  }

  int GetOrCreateRoom(const std::string& room_name) {
    // ルームを検索、無ければ作成
    std::lock_guard<std::mutex> guard( m_rooms_mutex );
    auto it = std::find_if(
      m_rooms.begin(), m_rooms.end(),
      [room_name](const std::map<int, RoomPtr>::value_type& pair)
      {
        return pair.second->room_name() == room_name;
      }
    );
    if (it != m_rooms.end()) {
      RoomPtr room = it->second;
      return room->room_id();
    }
    static int generated_room_id = 0;
    int room_id = ++generated_room_id;
    RoomPtr room = std::make_shared<Room>( room_id, room_name );
    m_rooms.emplace( room_id, room );
    return room_id;
  }

  RoomPtr FindRoom(int room_id) {
    std::lock_guard<std::mutex> lock( m_rooms_mutex );
    auto it = m_rooms.find(room_id);
    if (it == m_rooms.end()) return nullptr;
    return it->second;
  }

  Status JoinRoom(int room_id, const UserPtr& user) {
    std::lock_guard<std::mutex> guard( m_rooms_mutex );
    auto it = m_rooms.find(room_id);
    if (it == m_rooms.end()) {
      return Status(StatusCode::NOT_FOUND, "Room Not Found!");
    }
    RoomPtr& room = it->second;
    room->AddUser(user);
    std::cout << "user_id:" << user->user_id() << " room_id:" << room_id << " joined" << std::endl;
    return Status::OK;
  }

  std::vector<RoomPtr> GetAllRoom() {
    std::vector<RoomPtr> result;
    {
      std::lock_guard<std::mutex> guard( m_rooms_mutex );
      result.reserve( m_rooms.size() );
      for(auto& pair : m_rooms) {
        auto& room = pair.second;
        if (room->Empty()) continue;
        result.push_back( room );
      }
    }
    return result;
  }

  // TODO:ルーム削除

private:
  RoomMgr() = default;
  std::map<int, RoomPtr> m_rooms;
  std::mutex m_rooms_mutex;
};

// Logic and data behind the server's behavior.
class GreeterServiceImpl final : public Greeter::Service {
  Status CreateUser(ServerContext* context, const CreateUserRequest* request,
                  CreateUserReply* reply) override {
    int user_id = UserMgr::instance().CreateUser( request->user_name() );
    reply->set_user_id(user_id);
    return Status::OK;
  }

  Status GetRoomID(ServerContext* context, const GetRoomIDRequest* request,
                  GetRoomIDReply* reply) override {
    const std::string& room_name = request->room_name();
    int room_id = RoomMgr::instance().GetOrCreateRoom(room_name);
    reply->set_room_id( room_id );
    return Status::OK;
  }

  Status SendMessage(ServerContext* context, const ChatMessage* message,
                  google::protobuf::Empty* reply) override {
    std::cout << "recv room_id:" << message->room_id() << " [" << message->user_name() << "] " << message->message() << std::endl; // debug
    auto room = RoomMgr::instance().FindRoom( message->room_id() );
    if (!room) {
      return Status(StatusCode::NOT_FOUND, "Room Not Found");
    }
    room->RecvChat(*message);
    return Status::OK;
  }

  Status JoinRoom(ServerContext* context, const JoinRoomRequest* request,
                  ServerWriter<ChatMessage>* writer) override {
    auto user = UserMgr::instance().FindUser( request->user_id() );
    if (!user) {
      return Status(StatusCode::NOT_FOUND, "User Not Found");;
    }
    // roomに登録。以後発言がwriterに届くようになる
    auto status = RoomMgr::instance().JoinRoom( request->room_id(), user);
    if (!status.ok()) {
      return status;
    }
    user->set_writer(writer);
    user->set_receivable(true);
    while (user->is_receivable()) {
      sleep(1); // leaveするまで待機 // condition_variableによるnotifyとかを利用した方がいいのかもしれない
    }
    std::cout << "leave user_id:" << request->user_id() << " room_id:" << request->room_id() << std::endl; // debug
    return Status::OK; // writerへのストリーム終了
  }

  Status ListRooms(ServerContext* context, const google::protobuf::Empty* request,
                  ListRoomsReply* reply) override {
    auto rooms = RoomMgr::instance().GetAllRoom();
    for (const auto& room : rooms) {
      auto room_info = reply->add_rooms();
      room_info->set_room_id( room->room_id() );
      room_info->set_room_name( room->room_name() );
    }
    return Status::OK;
  }

  Status LeaveRoom(ServerContext* context, const LeaveRoomRequest* request,
                  google::protobuf::Empty* reply) override {
    int user_id = request->user_id();
    auto user = UserMgr::instance().FindUser( user_id );
    if (!user) {
      return Status(StatusCode::NOT_FOUND, "User Not Found!");
    }
    user->set_receivable(false);
    auto room = RoomMgr::instance().FindRoom( request->room_id() );
    if (!room) {
      return Status(StatusCode::NOT_FOUND, "Room Not Found!");
    }
    room->RemoveUser( user_id );
    return Status::OK;
  }
};

void RunServer() {
  std::string server_address("0.0.0.0:50051");
  GreeterServiceImpl service;

  grpc::EnableDefaultHealthCheckService(true);
  grpc::reflection::InitProtoReflectionServerBuilderPlugin();
  ServerBuilder builder;
  // Listen on the given address without any authentication mechanism.
  builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());
  // Register "service" as the instance through which we'll communicate with
  // clients. In this case it corresponds to an *synchronous* service.
  builder.RegisterService(&service);
  // Finally assemble the server.
  std::unique_ptr<Server> server(builder.BuildAndStart());
  std::cout << "Server listening on " << server_address << std::endl;

  // Wait for the server to shutdown. Note that some other thread must be
  // responsible for shutting down the server for this call to ever return.
  server->Wait();
}

int main(int argc, char** argv) {
  RunServer();

  return 0;
}
