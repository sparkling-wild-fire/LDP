#pragma once

#include <sstream>

#include "common/types.h"
#include "common/lf_queue.h"

using namespace Common;

namespace Exchange {
  /// Type of the order response sent by the exchange to the trading client.
  enum class ClientResponseType : uint8_t {
    INVALID = 0,
    ACCEPTED = 1,
    CANCELED = 2,
    FILLED = 3,
    CANCEL_REJECTED = 4
  };

  inline std::string clientResponseTypeToString(ClientResponseType type) {
    switch (type) {
      case ClientResponseType::ACCEPTED:
        return "ACCEPTED";
      case ClientResponseType::CANCELED:
        return "CANCELED";
      case ClientResponseType::FILLED:
        return "FILLED";
      case ClientResponseType::CANCEL_REJECTED:
        return "CANCEL_REJECTED";
      case ClientResponseType::INVALID:
        return "INVALID";
    }
    return "UNKNOWN";
  }

  /// These structures go over the wire / network, so the binary structures are packed to remove system dependent extra padding.
#pragma pack(push, 1)

  /// Client response structure used internally by the matching engine.
  struct MEClientResponse {
    ClientResponseType type_ = ClientResponseType::INVALID;     // 成交,撤单，失败，撤废...
    ClientId client_id_ = ClientId_INVALID;                     // 客户端id
    TickerId ticker_id_ = TickerId_INVALID;                     // 这个万一没看见有啥用啊
    OrderId client_order_id_ = OrderId_INVALID;                 // 委托号？
    OrderId market_order_id_ = OrderId_INVALID;                 // 市场号
    Side side_ = Side::INVALID;                                 // 委托方向
    Price price_ = Price_INVALID;                               // 价格
    Qty exec_qty_ = Qty_INVALID;                                // 成交数量？
    Qty leaves_qty_ = Qty_INVALID;                              // 未成交数量？

    auto toString() const {
      std::stringstream ss;
      ss << "MEClientResponse"
         << " ["
         << "type:" << clientResponseTypeToString(type_)
         << " client:" << clientIdToString(client_id_)
         << " ticker:" << tickerIdToString(ticker_id_)
         << " coid:" << orderIdToString(client_order_id_)
         << " moid:" << orderIdToString(market_order_id_)
         << " side:" << sideToString(side_)
         << " exec_qty:" << qtyToString(exec_qty_)
         << " leaves_qty:" << qtyToString(leaves_qty_)
         << " price:" << priceToString(price_)
         << "]";
      return ss.str();
    }
  };

  /// Client response structure published over the network by the order server.
  struct OMClientResponse {
    size_t seq_num_ = 0;
    MEClientResponse me_client_response_;

    auto toString() const {
      std::stringstream ss;
      ss << "OMClientResponse"
         << " ["
         << "seq:" << seq_num_
         << " " << me_client_response_.toString()
         << "]";
      return ss.str();
    }
  };

#pragma pack(pop) // Undo the packed binary structure directive moving forward.

  /// Lock free queues of matching engine client order response messages.
  typedef LFQueue<MEClientResponse> ClientResponseLFQueue;
}
