#include "market_data_consumer.h"

namespace Trading {
  MarketDataConsumer::MarketDataConsumer(Common::ClientId client_id, Exchange::MEMarketUpdateLFQueue *market_updates,
                                         const std::string &iface,
                                         const std::string &snapshot_ip, int snapshot_port,
                                         const std::string &incremental_ip, int incremental_port)
      : incoming_md_updates_(market_updates), run_(false),
        logger_("trading_market_data_consumer_" + std::to_string(client_id) + ".log"),
        incremental_mcast_socket_(logger_), snapshot_mcast_socket_(logger_),
        iface_(iface), snapshot_ip_(snapshot_ip), snapshot_port_(snapshot_port) {
            auto recv_callback = [this](auto socket) {
              recvCallback(socket);
    };
     // 两个套接字都用这个本类的回调函数，什么时候回调，sendAndRecv()回调
    incremental_mcast_socket_.recv_callback_ = recv_callback;
    ASSERT(incremental_mcast_socket_.init(incremental_ip, iface, incremental_port, /*is_listening*/ true) >= 0,
           "Unable to create incremental mcast socket. error:" + std::string(std::strerror(errno)));
    // 订阅这个套接字的多播流
    ASSERT(incremental_mcast_socket_.join(incremental_ip),
           "Join failed on:" + std::to_string(incremental_mcast_socket_.socket_fd_) + " error:" + std::string(std::strerror(errno)));

    snapshot_mcast_socket_.recv_callback_ = recv_callback;
  }

  /// Main loop for this thread - reads and processes messages from the multicast sockets - the heavy lifting is in the recvCallback() and checkSnapshotSync() methods.
  auto MarketDataConsumer::run() noexcept -> void {
    logger_.log("%:% %() %\n", __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_));
    while (run_) {
        // 消费队列，分发回调
        // 等待socket接收数据，调用回调函数recvCallback()处理数据
        // 完成快照更新就关闭socket了，那下次需要快照更新时，重新打开socket？没有啊，
        // 回调函数里会调用startSnapshotSync()方法，重新订阅快照流
        // 但是socket已经关闭了，不会再进入回调了，除非增量socket触发的回调让快照socket重新订阅？
        // 是的增量流socket的请求序号错误，会打开快照流订阅
      incremental_mcast_socket_.sendAndRecv();
      snapshot_mcast_socket_.sendAndRecv();
    }
  }

  /// Start the process of snapshot synchronization by subscribing to the snapshot multicast stream.
  auto MarketDataConsumer::startSnapshotSync() -> void {
      // 对来自快照和增量流的市场更新消息进行排队
    snapshot_queued_msgs_.clear();
    incremental_queued_msgs_.clear();

    ASSERT(snapshot_mcast_socket_.init(snapshot_ip_, iface_, snapshot_port_, /*is_listening*/ true) >= 0,
           "Unable to create snapshot mcast socket. error:" + std::string(std::strerror(errno)));
    // 对于多播套接字，我们不仅要确保有一个正在读取市场数据的套接字，还必须发出IGMP加入成员关系的网络级消息，这样消息才能流向应用程序?
    //         创建多播套接字，通过IGMP发送给路由器，通知网络设备加入多播组，网络设备可能不会将多播数据转发给该主机
    // 让指定的套接字加入到指定的多播组，以便接收发往该多播组的数据。
    // 多播允许多个接收者同时接收相同的数据流，而发送者只需发送一份数据，网络设备（如路由器）负责复制和转发数据包到所有注册的接收者。
    // 这种方式显著节省了网络带宽。
    // 多播常用于需要向多个接收者同时发送相同数据的场景，如视频会议、在线直播、实时股票市场数据分发、远程教育等。
    ASSERT(snapshot_mcast_socket_.join(snapshot_ip_), // IGMP multicast subscription.
           "Join failed on:" + std::to_string(snapshot_mcast_socket_.socket_fd_) + " error:" + std::string(std::strerror(errno)));
  }

  /// Check if a recovery / synchronization is possible from the queued up market data updates from the snapshot and incremental market data streams.
  auto MarketDataConsumer::checkSnapshotSync() -> void {
    if (snapshot_queued_msgs_.empty()) {
      return;
    }

    const auto &first_snapshot_msg = snapshot_queued_msgs_.begin()->second;
    if (first_snapshot_msg.type_ != Exchange::MarketUpdateType::SNAPSHOT_START) {
      logger_.log("%:% %() % Returning because have not seen a SNAPSHOT_START yet.\n",
                  __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_));
      snapshot_queued_msgs_.clear();
      return;
    }

    std::vector<Exchange::MEMarketUpdate> final_events;

    auto have_complete_snapshot = true;
    size_t next_snapshot_seq = 0;
    for (auto &snapshot_itr: snapshot_queued_msgs_) {
      logger_.log("%:% %() % % => %\n", __FILE__, __LINE__, __FUNCTION__,
                  Common::getCurrentTimeStr(&time_str_), snapshot_itr.first, snapshot_itr.second.toString());
      if (snapshot_itr.first != next_snapshot_seq) {
        have_complete_snapshot = false;
        logger_.log("%:% %() % Detected gap in snapshot stream expected:% found:% %.\n", __FILE__, __LINE__, __FUNCTION__,
                    Common::getCurrentTimeStr(&time_str_), next_snapshot_seq, snapshot_itr.first, snapshot_itr.second.toString());
        break;
      }

      if (snapshot_itr.second.type_ != Exchange::MarketUpdateType::SNAPSHOT_START &&
          snapshot_itr.second.type_ != Exchange::MarketUpdateType::SNAPSHOT_END)
        final_events.push_back(snapshot_itr.second);

      ++next_snapshot_seq;
    }

    if (!have_complete_snapshot) {
      logger_.log("%:% %() % Returning because found gaps in snapshot stream.\n",
                  __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_));
      snapshot_queued_msgs_.clear();
      return;
    }

    const auto &last_snapshot_msg = snapshot_queued_msgs_.rbegin()->second;
    if (last_snapshot_msg.type_ != Exchange::MarketUpdateType::SNAPSHOT_END) {
      logger_.log("%:% %() % Returning because have not seen a SNAPSHOT_END yet.\n",
                  __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_));
      return;
    }

    auto have_complete_incremental = true;
    size_t num_incrementals = 0;
      // 为啥next_exp_inc_seq_num_要和order_id_关联？增量流量的key是orderid?
    next_exp_inc_seq_num_ = last_snapshot_msg.order_id_ + 1;
    for (auto inc_itr = incremental_queued_msgs_.begin(); inc_itr != incremental_queued_msgs_.end(); ++inc_itr) {
      logger_.log("%:% %() % Checking next_exp:% vs. seq:% %.\n", __FILE__, __LINE__, __FUNCTION__,
                  Common::getCurrentTimeStr(&time_str_), next_exp_inc_seq_num_, inc_itr->first, inc_itr->second.toString());

      // inc_itr的first是seq_num,增量流的发布，seq_num和order_id有啥关系？
      if (inc_itr->first < next_exp_inc_seq_num_)
        continue;

      if (inc_itr->first != next_exp_inc_seq_num_) {
        logger_.log("%:% %() % Detected gap in incremental stream expected:% found:% %.\n", __FILE__, __LINE__, __FUNCTION__,
                    Common::getCurrentTimeStr(&time_str_), next_exp_inc_seq_num_, inc_itr->first, inc_itr->second.toString());
        have_complete_incremental = false;
        break;
      }

      logger_.log("%:% %() % % => %\n", __FILE__, __LINE__, __FUNCTION__,
                  Common::getCurrentTimeStr(&time_str_), inc_itr->first, inc_itr->second.toString());

      if (inc_itr->second.type_ != Exchange::MarketUpdateType::SNAPSHOT_START &&
          inc_itr->second.type_ != Exchange::MarketUpdateType::SNAPSHOT_END)
        final_events.push_back(inc_itr->second);

      ++next_exp_inc_seq_num_;
      ++num_incrementals;
    }

    if (!have_complete_incremental) {
      logger_.log("%:% %() % Returning because have gaps in queued incrementals.\n",
                  __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_));
      snapshot_queued_msgs_.clear();
      return;
    }
    // 写入incoming_md_updates_无锁队列，以便发送到交易引擎组件
    for (const auto &itr: final_events) {
      auto next_write = incoming_md_updates_->getNextToWriteTo();
      *next_write = itr;
      incoming_md_updates_->updateWriteIndex();
    }

    logger_.log("%:% %() % Recovered % snapshot and % incremental orders.\n", __FILE__, __LINE__, __FUNCTION__,
                Common::getCurrentTimeStr(&time_str_), snapshot_queued_msgs_.size() - 2, num_incrementals);

    snapshot_queued_msgs_.clear();
    incremental_queued_msgs_.clear();      // 如果没有快照同步，增量流量应该是取一个消费一个吧
    in_recovery_ = false;
    // 不再需要订阅快照流，也不需要接收或处理快照消息
    snapshot_mcast_socket_.leave(snapshot_ip_, snapshot_port_);
  }

  /// Queue up a message in the *_queued_msgs_ containers, first parameter specifies if this update came from the snapshot or the incremental streams.
  auto MarketDataConsumer::queueMessage(bool is_snapshot, const Exchange::MDPMarketUpdate *request) {
    if (is_snapshot) {
      if (snapshot_queued_msgs_.find(request->seq_num_) != snapshot_queued_msgs_.end()) {
        logger_.log("%:% %() % Packet drops on snapshot socket. Received for a 2nd time:%\n", __FILE__, __LINE__, __FUNCTION__,
                    Common::getCurrentTimeStr(&time_str_), request->toString());
        snapshot_queued_msgs_.clear();
      }
      snapshot_queued_msgs_[request->seq_num_] = request->me_market_update_;
    } else {
      incremental_queued_msgs_[request->seq_num_] = request->me_market_update_;
    }

    logger_.log("%:% %() % size snapshot:% incremental:% % => %\n", __FILE__, __LINE__, __FUNCTION__,
                Common::getCurrentTimeStr(&time_str_), snapshot_queued_msgs_.size(), incremental_queued_msgs_.size(), request->seq_num_, request->toString());

    checkSnapshotSync();
  }

  /// Process a market data update, the consumer needs to use the socket parameter to figure out whether this came from the snapshot or the incremental stream.
  auto MarketDataConsumer::recvCallback(McastSocket *socket) noexcept -> void {
    TTT_MEASURE(T7_MarketDataConsumer_UDP_read, logger_);

    START_MEASURE(Trading_MarketDataConsumer_recvCallback);
    const auto is_snapshot = (socket->socket_fd_ == snapshot_mcast_socket_.socket_fd_);
    if (UNLIKELY(is_snapshot && !in_recovery_)) { // market update was read from the snapshot market data stream and we are not in recovery, so we dont need it and discard it.
      socket->next_rcv_valid_index_ = 0;

      logger_.log("%:% %() % WARN Not expecting snapshot messages.\n",
                  __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_));

      return;
    }

    if (socket->next_rcv_valid_index_ >= sizeof(Exchange::MDPMarketUpdate)) {
      size_t i = 0;
      for (; i + sizeof(Exchange::MDPMarketUpdate) <= socket->next_rcv_valid_index_; i += sizeof(Exchange::MDPMarketUpdate)) {
        auto request = reinterpret_cast<const Exchange::MDPMarketUpdate *>(socket->inbound_data_.data() + i);
        logger_.log("%:% %() % Received % socket len:% %\n", __FILE__, __LINE__, __FUNCTION__,
                    Common::getCurrentTimeStr(&time_str_),
                    (is_snapshot ? "snapshot" : "incremental"), sizeof(Exchange::MDPMarketUpdate), request->toString());

        const bool already_in_recovery = in_recovery_;
        in_recovery_ = (already_in_recovery || request->seq_num_ != next_exp_inc_seq_num_);

        if (UNLIKELY(in_recovery_)) {         // 在恢复中
          if (UNLIKELY(!already_in_recovery)) { // if we just entered recovery, start the snapshot synchonization process by subscribing to the snapshot multicast stream.
            logger_.log("%:% %() % Packet drops on % socket. SeqNum expected:% received:%\n", __FILE__, __LINE__, __FUNCTION__,
                        Common::getCurrentTimeStr(&time_str_), (is_snapshot ? "snapshot" : "incremental"), next_exp_inc_seq_num_, request->seq_num_);
            // startSnapshotSync()方法将初始化snapshot_mcast_socket_对象并订阅快照多播流
            startSnapshotSync();
          }
            // 调用queueMessage()方法来存储刚刚收到的MDPMarketUpdate消息
          queueMessage(is_snapshot, request); // queue up the market data update message and check if snapshot recovery / synchronization can be completed successfully.
          //们保持在恢复模式，并在快照和增量流上排队市场数据更新。
          // 在从快照流获得完整的订单簿快照，以及在快照消息之后的所有增量消息以跟上增量流之前，我们都会这样做
        } else if (!is_snapshot) { // not in recovery and received a packet in the correct order and without gaps, process it.
            // 写增量数据
            logger_.log("%:% %() % %\n", __FILE__, __LINE__, __FUNCTION__,
                      Common::getCurrentTimeStr(&time_str_), request->toString());

          ++next_exp_inc_seq_num_;

          auto next_write = incoming_md_updates_->getNextToWriteTo();
          *next_write = std::move(request->me_market_update_);
          incoming_md_updates_->updateWriteIndex();
          TTT_MEASURE(T8_MarketDataConsumer_LFQueue_write, logger_);
        }
      }
      memcpy(socket->inbound_data_.data(), socket->inbound_data_.data() + i, socket->next_rcv_valid_index_ - i);
      socket->next_rcv_valid_index_ -= i;
    }
    END_MEASURE(Trading_MarketDataConsumer_recvCallback, logger_);
  }
}
