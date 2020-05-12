// Copyright [2020] <Copyright Kevin, kevin.lau.gd@gmail.com>

#include "TradingEngine.h"

#include "AlgoTrade/TraderProtocol.h"
#include "ContractTable.h"

namespace ft {

TradingEngine::TradingEngine()
    : portfolio_("127.0.0.1", 6379), tick_redis_("127.0.0.1", 6379) {}

TradingEngine::~TradingEngine() {}

bool TradingEngine::login(const LoginParams& params) {
  if (is_logon_) return true;

  gateway_.reset(create_gateway(params.api(), this));
  if (!gateway_) {
    spdlog::error("[TradingEngine::login] Failed. Unknown gateway");
    return false;
  }

  if (!gateway_->login(params)) {
    spdlog::error("[TradingEngine::login] Failed to login");
    return false;
  }

  spdlog::info("[TradingEngine::login] Success. Login as {}",
               params.investor_id());

  if (!gateway_->query_account()) {
    spdlog::error("[TradingEngine::login] Failed to query account");
    return false;
  }

  // query all positions
  if (!gateway_->query_positions()) {
    spdlog::error("[TradingEngine::login] Failed to query positions");
    return false;
  }

  std::this_thread::sleep_for(std::chrono::seconds(1));

  while (!is_logon_) continue;
  return true;
}

void TradingEngine::run() {
  order_redis_.subscribe({TRADER_CMD_TOPIC});

  for (;;) {
    auto reply = order_redis_.get_sub_reply();
    auto cmd = reinterpret_cast<const TraderCommand*>(reply->element[2]->str);
    switch (cmd->type) {
      case NEW_ORDER:
        send_order(cmd->order_req.ticker_index, cmd->order_req.volume,
                   cmd->order_req.direction, cmd->order_req.offset,
                   cmd->order_req.type, cmd->order_req.price);
        break;
      case CANCEL_ORDER:
        if (cmd->cancel_req.order_id != 0)
          cancel_order(cmd->cancel_req.order_id);
        break;
      default:
        spdlog::error("[StrategyEngine::run] Unknown cmd");
        break;
    }
  }
}

uint64_t TradingEngine::send_order(uint64_t ticker_index, int volume,
                                   uint64_t direction, uint64_t offset,
                                   uint64_t type, double price) {
  if (!is_logon_) {
    spdlog::error("[TradingEngine::send_order] Failed. Not logon");
    return 0;
  }

  auto contract = ContractTable::get_by_index(ticker_index);
  if (!contract) {
    spdlog::error("[TradingEngine::send_order] Contract not found");
    return 0;
  }

  Order order;
  order.ticker_index = ticker_index;
  order.direction = direction;
  order.offset = offset;
  order.volume = volume;
  order.type = type;
  order.price = price;
  order.status = OrderStatus::SUBMITTING;

  OrderReq req;
  req.ticker_index = ticker_index;
  req.direction = direction;
  req.offset = offset;
  req.volume = volume;
  req.type = type;
  req.price = price;

  std::unique_lock<std::mutex> lock(mutex_);
  if (!risk_mgr_.check(&order)) {
    spdlog::error("[StrategyEngine::send_order] RiskMgr check failed");
    return 0;
  }

  order.order_id = gateway_->send_order(&req);
  if (order.order_id == 0) {
    spdlog::error(
        "[StrategyEngine::send_order] Failed to send_order."
        " Order: <Ticker: {}, OrderID: {}, Direction: {}, "
        "Offset: {}, OrderType: {}, Traded: {}, Total: {}, Price: {:.2f}, "
        "Status: {}>",
        contract->ticker, order.order_id, direction_str(order.direction),
        offset_str(order.offset), ordertype_str(order.type), 0, order.volume,
        order.price, to_string(order.status));
    return 0;
  }

  // panel_.process_new_order(&order);
  order_map_.emplace(order.order_id, order);
  portfolio_.update_pending(contract->index, direction, offset, volume);

  spdlog::debug(
      "[StrategyEngine::send_order] Success."
      " Order: <Ticker: {}, OrderID: {}, Direction: {}, "
      "Offset: {}, OrderType: {}, Traded: {}, Total: {}, Price: {:.2f}, "
      "Status: {}>",
      contract->ticker, order.order_id, direction_str(order.direction),
      offset_str(order.offset), ordertype_str(order.type), 0, order.volume,
      order.price, to_string(order.status));
  return order.order_id;
}

void TradingEngine::cancel_order(uint64_t order_id) {}

void TradingEngine::cancel_all_by_ticker(const std::string& ticker) {}

void TradingEngine::cancel_all() {}

void TradingEngine::on_query_contract(const Contract* contract) {}

void TradingEngine::on_query_account(const Account* account) {
  spdlog::info(
      "[TradingEngine::on_query_account] Account ID: {}, Balance: {}, Fronzen: "
      "{}",
      account->account_id, account->balance, account->frozen);
}

void TradingEngine::on_query_position(const Position* position) {
  auto contract = ContractTable::get_by_index(position->ticker_index);
  assert(contract);

  auto& lp = position->long_pos;
  auto& sp = position->short_pos;
  spdlog::info(
      "[TradingEngine::on_query_position] Ticker: {}, "
      "Long Volume: {}, Long Price: {:.2f}, Long Frozen: {}, Long PNL: {}, "
      "Short Volume: {}, Short Price: {:.2f}, Short Frozen: {}, Short PNL: {}",
      contract->ticker, lp.volume, lp.cost_price, lp.frozen, lp.float_pnl,
      sp.volume, sp.cost_price, sp.frozen, sp.float_pnl);

  if (lp.volume == 0 && lp.frozen == 0 && sp.volume == 0 && sp.frozen == 0)
    return;

  portfolio_.set_position(position);
}

void TradingEngine::on_tick(const TickData* tick) {
  auto contract = ContractTable::get_by_index(tick->ticker_index);
  if (!contract) {
    spdlog::error("[TradingEngine::process_tick] Contract not found");
    return;
  }

  tick_redis_.publish(get_md_topic(contract->ticker), tick, sizeof(TickData));
  spdlog::debug("[TradingEngine::process_tick]");
}

void TradingEngine::on_order_accepted(uint64_t order_id) {
  std::unique_lock<std::mutex> lock(mutex_);
  auto iter = order_map_.find(order_id);
  if (iter == order_map_.end()) {
    return;
  }

  auto& order = iter->second;
}

void TradingEngine::on_order_rejected(uint64_t order_id) {
  std::unique_lock<std::mutex> lock(mutex_);
  auto iter = order_map_.find(order_id);
  if (iter == order_map_.end()) {
    return;
  }

  auto& order = iter->second;
  order_map_.erase(iter);
}

void TradingEngine::on_order_traded(uint64_t order_id, int64_t this_traded,
                                    double traded_price) {
  std::unique_lock<std::mutex> lock(mutex_);
  auto iter = order_map_.find(order_id);
  if (iter == order_map_.end()) {
    return;
  }

  auto& order = iter->second;
  portfolio_.update_traded(order.ticker_index, order.direction, order.offset,
                           this_traded, traded_price);

  order.traded_volume += this_traded;
  if (order.traded_volume + order.canceled_volume == order.volume) {
    order_map_.erase(iter);
  }
}

void TradingEngine::on_order_canceled(uint64_t order_id,
                                      int64_t canceled_volume) {
  std::unique_lock<std::mutex> lock(mutex_);
  auto iter = order_map_.find(order_id);
  if (iter == order_map_.end()) {
    return;
  }

  auto& order = iter->second;
  order.canceled_volume = canceled_volume;
  if (order.traded_volume + order.canceled_volume == order.volume) {
    order_map_.erase(iter);
  }
}

void TradingEngine::on_order_cancel_rejected(uint64_t order_id) {}

}  // namespace ft
