#include "market_data_publisher.h"

namespace Exchange {
    /****
     * 增量市场数据流：
     *      该数据流仅发布订单簿先前状态的增量更新信息，这意味着此数据流对带宽的要求要低得多，因为它仅发布订单簿的增量更新内容。
     *      在正常运行条件下，市场参与者只需订阅增量数据流，就能维持订单簿的正确状态
     * 快照市场数据流:
     *      包含的数据可用于从完全空白的状态构建完整的订单簿
     *      由于该数据流包含每个交易工具订单簿中所有订单的信息，其带宽占用可能会很大,这意味着可能每隔几秒才发送一次快照消息流
     */
     /****
      * `MarketDataPublisher`类包含以下重要成员：
        - 一个`size_t`类型的`next_inc_seq_num_`变量，它表示要设置在下一个传出的增量市场数据消息上的序列号。
        - 一个`MEMarketUpdateLFQueue`类型的`outgoing_md_updates_`变量，它是一个`MEMarketUpdate`消息的无锁队列。
        这个无锁队列是匹配引擎向市场数据发布者发送`MEMarketUpdate`消息的途径，市场数据发布者随后会通过UDP发布这些消息。
        - 一个`incremental_socket_`成员，它是一个`McastSocket`，用于在增量多播流上发布UDP消息。
        - 一个`SnapshotSynthesizer`类型的`snapshot_synthesizer_`变量。负责根据匹配引擎提供的更新生成限价订单簿的快照，并定期在快照多播流上发布完整订单簿的快照。
        - 一个名为`snapshot_md_updates_`的无锁队列实例，它的类型为`MDPMarketUpdateLFQueue`，是一个包含`MDPMarketUpdate`消息的无锁队列。
        市场数据发布者线程使用这个队列，将其在增量流上发送的`MDPMarketUpdate`消息发布给`SnapshotSynthesizer`组件。
        这个无锁队列是必要的，因为`SnapshotSynthesizer`与`MarketDataPublisher`在不同的线程上运行，
        这主要是为了确保快照合成和发布过程不会降低对延迟敏感的`MarketDataPublisher`组件的性能。

        namespace Exchange {
        class MarketDataPublisher {
        private:
            size_t next_inc_seq_num_ = 1;       // 下一个传出的增量市场数据消息上的序列号
            MEMarketUpdateLFQueue *outgoing_md_updates_ = nullptr;          // MEMarketUpdate无锁队列,匹配引擎向发布者发送消息
            MDPMarketUpdateLFQueue snapshot_md_updates_;                    // 发布者线程使用这个队列，将其在增量流上发送的`MDPMarketUpdate`消息发布给`SnapshotSynthesizer`组件。
            volatile bool run_ = false;
            std::string time_str_;
            Logger logger_;
            Common::McastSocket incremental_socket_;                       // 在增量多播流上发布UDP消息
            SnapshotSynthesizer *snapshot_synthesizer_ = nullptr;           // 根据匹配引擎提供的更新生成限价订单簿的快照,并发布快照
        };
        }
      */
  MarketDataPublisher::MarketDataPublisher(MEMarketUpdateLFQueue *market_updates, const std::string &iface,
                                           const std::string &snapshot_ip, int snapshot_port,
                                           const std::string &incremental_ip, int incremental_port)
      : outgoing_md_updates_(market_updates), snapshot_md_updates_(ME_MAX_MARKET_UPDATES),
        run_(false), logger_("exchange_market_data_publisher.log"), incremental_socket_(logger_) {
    ASSERT(incremental_socket_.init(incremental_ip, iface, incremental_port, /*is_listening*/ false) >= 0,
           "Unable to create incremental mcast socket. error:" + std::string(std::strerror(errno)));
    snapshot_synthesizer_ = new SnapshotSynthesizer(&snapshot_md_updates_, iface, snapshot_ip, snapshot_port);
  }

  /// Main run loop for this thread - consumes market updates from the lock free queue from the matching engine, publishes them on the incremental multicast stream and forwards them to the snapshot synthesizer.
  auto MarketDataPublisher::run() noexcept -> void {
    logger_.log("%:% %() %\n", __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_));
    while (run_) {
        // 遍历匹配引擎发布的任何新的MEMarketDataUpdates消息，清空outgoing_md_updates_队列
      for (auto market_update = outgoing_md_updates_->getNextToRead();
           outgoing_md_updates_->size() && market_update; market_update = outgoing_md_updates_->getNextToRead()) {
        TTT_MEASURE(T5_MarketDataPublisher_LFQueue_read, logger_);

        logger_.log("%:% %() % Sending seq:% %\n", __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_), next_inc_seq_num_,
                    market_update->toString().c_str());

        START_MEASURE(Exchange_McastSocket_send);
        // 为啥两个ip+port,一个UDP socket
        incremental_socket_.send(&next_inc_seq_num_, sizeof(next_inc_seq_num_));
        incremental_socket_.send(market_update, sizeof(MEMarketUpdate));
        END_MEASURE(Exchange_McastSocket_send, logger_);

        outgoing_md_updates_->updateReadIndex();
        TTT_MEASURE(T6_MarketDataPublisher_UDP_write, logger_);

        // Forward this incremental market data update the snapshot synthesizer.
        // 写入套接字的相同增量更新写入snapshot_md_updates_无锁队列，以通知SnapshotSynthesizer组件
        auto next_write = snapshot_md_updates_.getNextToWriteTo();
        next_write->seq_num_ = next_inc_seq_num_;
        next_write->me_market_update_ = *market_update;
        snapshot_md_updates_.updateWriteIndex();

        ++next_inc_seq_num_;
      }

      // Publish to the multicast stream.
      // todo:上面的send不久发送了吗，sendAndRecv干嘛？
      incremental_socket_.sendAndRecv();
    }
  }
}
