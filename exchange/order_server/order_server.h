#pragma once

#include <functional>

#include "common/thread_utils.h"
#include "common/macros.h"
#include "common/tcp_server.h"

#include "order_server/client_request.h"
#include "order_server/client_response.h"
#include "order_server/fifo_sequencer.h"

namespace Exchange {
  class OrderServer {
  public:
    OrderServer(ClientRequestLFQueue *client_requests, ClientResponseLFQueue *client_responses, const std::string &iface, int port);

    ~OrderServer();

    /// Start and stop the order server main thread.
    auto start() -> void;

    auto stop() -> void;

    /// Main run loop for this thread - accepts new client connections,
    /// receives client requests from them and sends client responses to them.
    auto run() noexcept {
      logger_.log("%:% %() %\n", __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_));
      while (run_) {
        tcp_server_.poll();
        // sendAndRecv()方法从每个TCP连接读取数据，并为它们调度回调
        // 分派完所有recvCallback()方法后，就会调用recvFinishedCallback()方法
        tcp_server_.sendAndRecv();

        // 这里就是处理响应,也就是将匹配引擎的响应发送给客户端
        // outgoing_responses_也是动态变化的吗，也就是一边发送，一边有数据进来
        for (auto client_response = outgoing_responses_->getNextToRead(); outgoing_responses_->size() && client_response; client_response = outgoing_responses_->getNextToRead()) {
          TTT_MEASURE(T5t_OrderServer_LFQueue_read, logger_);
          // cid_next_outgoing_seq_num_获取序列号，发送消息的序列号和请求的序列号要一致吗？
          auto &next_outgoing_seq_num = cid_next_outgoing_seq_num_[client_response->client_id_];
          logger_.log("%:% %() % Processing cid:% seq:% %\n", __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_),
                      client_response->client_id_, next_outgoing_seq_num, client_response->toString());

          ASSERT(cid_tcp_socket_[client_response->client_id_] != nullptr,
                 "Dont have a TCPSocket for ClientId:" + std::to_string(client_response->client_id_));
          START_MEASURE(Exchange_TCPSocket_send);
          // 匹配引擎的响应，先发送序列号，再发送响应体？
          // OMClientResponse = MEClientResponse + next_outgoing_seq_num_
          cid_tcp_socket_[client_response->client_id_]->send(&next_outgoing_seq_num, sizeof(next_outgoing_seq_num));
          cid_tcp_socket_[client_response->client_id_]->send(client_response, sizeof(MEClientResponse));
          END_MEASURE(Exchange_TCPSocket_send, logger_);

          outgoing_responses_->updateReadIndex();
          TTT_MEASURE(T6t_OrderServer_TCP_write, logger_);

          ++next_outgoing_seq_num;
        }
      }
    }

    /// Read client request from the TCP receive buffer, check for sequence gaps and forward it to the FIFO sequencer.
    auto recvCallback(TCPSocket *socket, Nanos rx_time) noexcept {
      TTT_MEASURE(T1_OrderServer_TCP_read, logger_);
      logger_.log("%:% %() % Received socket:% len:% rx:%\n", __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_),
                  socket->socket_fd_, socket->next_rcv_valid_index_, rx_time);
      // 检查可用数据的大小是否至少与一个完整的OMClientRequest结构体一样大,也就是接收缓冲区能否存储OMClientRequest
      // 但是不是来一个请求就会处理吗 => orderServer不会是单线程吧。。。
      if (socket->next_rcv_valid_index_ >= sizeof(OMClientRequest)) {
        size_t i = 0;
        for (; i + sizeof(OMClientRequest) <= socket->next_rcv_valid_index_; i += sizeof(OMClientRequest)) {
          // 取socket的用户态读缓冲区,将TCPSocket中的rcv_buffer_重新解释为OMClientRequest结构体，并将其保存在request变量中
          // todo:你给我说客户端id是传进来的？还不如用session,然后session,socket映射
          auto request = reinterpret_cast<const OMClientRequest *>(socket->inbound_data_.data() + i);
          logger_.log("%:% %() % Received %\n", __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_), request->toString());

          if (UNLIKELY(cid_tcp_socket_[request->me_client_request_.client_id_] == nullptr)) { // first message from this ClientId.
              // 不是连接建立，而是第一个消息收到的时候维护
            cid_tcp_socket_[request->me_client_request_.client_id_] = socket;
          }

          if (cid_tcp_socket_[request->me_client_request_.client_id_] != socket) { // TODO - change this to send a reject back to the client.
            logger_.log("%:% %() % Received ClientRequest from ClientId:% on different socket:% expected:%\n", __FILE__, __LINE__, __FUNCTION__,
                        Common::getCurrentTimeStr(&time_str_), request->me_client_request_.client_id_, socket->socket_fd_,
                        cid_tcp_socket_[request->me_client_request_.client_id_]->socket_fd_);
            continue;
          }

          // socket中传入的请求序号不是希望得到的请求序号，每个客户端的请求号都必须递增
          // 超过最大值怎么办，如类型的最大值
          auto &next_exp_seq_num = cid_next_exp_seq_num_[request->me_client_request_.client_id_];
          if (request->seq_num_ != next_exp_seq_num) { // TODO - change this to send a reject back to the client.
            logger_.log("%:% %() % Incorrect sequence number. ClientId:% SeqNum expected:% received:%\n", __FILE__, __LINE__, __FUNCTION__,
                        Common::getCurrentTimeStr(&time_str_), request->me_client_request_.client_id_, next_exp_seq_num, request->seq_num_);
            continue;
          }

          ++next_exp_seq_num;

          START_MEASURE(Exchange_FIFOSequencer_addClientRequest);
          // 这里取出来了，应该还要做一个转换，内部结构转换为交易所格式
          fifo_sequencer_.addClientRequest(rx_time, request->me_client_request_);
          END_MEASURE(Exchange_FIFOSequencer_addClientRequest, logger_);
        }
        // 目标起始地址，源起始地址，源长度,data()返回vector的起始地址
        memcpy(socket->inbound_data_.data(), socket->inbound_data_.data() + i, socket->next_rcv_valid_index_ - i);
        // todo:不担心处理请求的时候，又来了请求吗？是兼容了的吧
        socket->next_rcv_valid_index_ -= i;
      }
    }

    /// End of reading incoming messages across all the TCP connections, sequence and publish the client requests to the matching engine.
    // 什么时候触发呢？
    auto recvFinishedCallback() noexcept {
      START_MEASURE(Exchange_FIFOSequencer_sequenceAndPublish);
      // 所有请求处理完了，就进行FIFO队列的排序
      fifo_sequencer_.sequenceAndPublish();
      END_MEASURE(Exchange_FIFOSequencer_sequenceAndPublish, logger_);
    }

    /// Deleted default, copy & move constructors and assignment-operators.
    OrderServer() = delete;

    OrderServer(const OrderServer &) = delete;

    OrderServer(const OrderServer &&) = delete;

    OrderServer &operator=(const OrderServer &) = delete;

    OrderServer &operator=(const OrderServer &&) = delete;

  private:
    const std::string iface_;
    const int port_ = 0;

    /// Lock free queue of outgoing client responses to be sent out to connected clients.
    // 接收交易引擎的MEClientResponse消息，这些消息需要发送给正确的市场参与者
    ClientResponseLFQueue *outgoing_responses_ = nullptr;

    volatile bool run_ = false;

    std::string time_str_;
    Logger logger_;

    /// Hash map from ClientId -> the next sequence number to be sent on outgoing client responses.
    // 跟踪OMClientResponse和OMClientRequest消息上的交易所到客户端和客户端到交易所的序列号
    std::array<size_t, ME_MAX_NUM_CLIENTS> cid_next_outgoing_seq_num_;
    std::array<size_t, ME_MAX_NUM_CLIENTS> cid_next_exp_seq_num_;

    /// Hash map from ClientId -> TCP socket / client connection.
    // 含ME_MAX_NUM_CLIENTS个TCPSocket对象，它将用作从客户端ID到该客户端的TCPSocket连接的哈希映射。
    // record:这不是一个数组吗？  应该是orderServer给连接进来的客户端一个自增的id,这个id作为数组索引，value为socket,一个客户端一个ordersocket
    // todo:应该在新建连接的时候确定吧
    std::array<Common::TCPSocket *, ME_MAX_NUM_CLIENTS> cid_tcp_socket_;

    /// TCP server instance listening for new client connections.
    Common::TCPServer tcp_server_;

    /// FIFO sequencer responsible for making sure incoming client requests are processed in the order in which they were received.
    // todo:先进先出队列，多个socket的请求按序发给匹配引擎，话说先插入队列的不就ok了，为啥还要排序，本来就是按时间排好了啊
    // 多个socket上的请求都发送到了这个队列了
    FIFOSequencer fifo_sequencer_;
  };
}
