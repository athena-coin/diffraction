#include <eosio/eosio.hpp>
#include <eosio/asset.hpp>
#include <eosio/symbol.hpp>
#include <eosio/system.hpp>
#include <eosio/action.hpp>
using namespace eosio;
using namespace std;

class [[eosio::contract]] diffraction : public contract {
public:
    using contract::contract;
    diffraction(name receiver, name code, datastream<const char*> ds):contract(receiver, code, ds), statetable(_self, _self.value) {
        // each time the contract is called, check 'state' table first
        auto it = statetable.find(1);
        if (it == statetable.end()) {
            statetable.emplace(_self, [&]( auto& row ) {
                row.round_remain = asset(LEVEL, TOK_SYM);
                row.round = 1;
                row.total_send = asset(0, TOK_SYM);
                row.total_received = asset(0, EOS_SYM);
                row.rate = INIT_RATE;
                row.available = false;
            });
        }
    }
    // monitor all EOS sent to this contract
    [[eosio::on_notify("eosio.token::transfer")]]
    void on_eos_transfer(name from, name to, asset quantity, std::string memo) {
        if (from == _self || to != _self) {
            return;
        }
        check(quantity.symbol == EOS_SYM, "we only need eos");
        check(quantity.is_valid(), "invalid quantity" );
        check(quantity.amount > 0, "invalid quantity");
        check(memo.size() <= 256, "memo has more than 256 bytes" );
        const auto sa = get_send_amount(from, quantity.amount);
        asset token_send(sa, TOK_SYM);
        action(permission_level{_self, "active"_n},
            TOKEN_CONTRACT, "transfer"_n,
            make_tuple(_self, from, token_send, string("diffraction"))
        ).send();
    }
    // clear 'state' table
    [[eosio::action]]
    void cleartable() {
        require_auth(permission_level{_self, "active"_n});
        auto it = statetable.begin();
        while (it != statetable.end()) {
            it = statetable.erase(it);
        }
    }
    // start diffraction
    [[eosio::action]]
    void start() {
        require_auth(permission_level{_self, "active"_n});
        const auto& stat = statetable.get(1);
        statetable.modify(stat, _self, [] (auto& row) {
            row.available = true;
        });
    }
    // stop diffraction
    [[eosio::action]]
    void stop() {
        require_auth(permission_level{_self, "active"_n});
        const auto& stat = statetable.get(1);
        statetable.modify(stat, _self, [] (auto& row) {
            row.available = false;
        });
    }

private:
    const symbol EOS_SYM = symbol(symbol_code("EOS"), 4); // EOS symbol
    const symbol TOK_SYM = symbol(symbol_code("ATHENA"), 4); // token symbol
    const name TOKEN_CONTRACT = "athenastoken"_n; // token contract
    const name EOS_CONTRACT = "eosio.token"_n; // eos contract
    const int64_t LEVEL = 3000000000; 
    const double RATE_ACC = 1.015; 
    const int64_t SEND_LIMIT = 790030000000;
    const double INIT_RATE = 100.0 / 3.0;

    struct [[eosio::table]] state {
        asset round_remain; 
        int16_t round;      
        asset total_send;   
        asset total_received; 
        double rate;        
        bool available; 
        uint64_t primary_key() const { return 1; }
    };
    typedef eosio::multi_index<"state"_n, state> state_index;
    state_index statetable;

    int64_t get_send_amount(name from, int64_t received) {
        const auto& stat = statetable.get(1);
        int64_t sa = 0; // send amount
        int64_t eos_remain = received; 
        auto rate = stat.rate;
        auto remain = stat.round_remain;
        auto round = stat.round;
        auto total_send = stat.total_send; // token send
        auto total_received = stat.total_received; // eos received
        auto total_remain = SEND_LIMIT - total_send.amount; //token remain
        bool is_complete = false; 
        check(stat.available, "diffraction not yet started or has ended");
        if (remain.amount > total_remain) { 
            remain.amount = total_remain;
        }
        while((int64_t)(eos_remain * rate) >= remain.amount) { 
            eos_remain -= remain.amount / rate;
            sa += remain.amount;
            total_remain -=remain.amount;
            if (total_remain <= 0) { 
                is_complete = true;
                break;
            }
            if (total_remain < LEVEL) { 
                remain.amount = total_remain;
            } else {
                remain.amount = LEVEL;
            }
            round++;
            rate = rate / RATE_ACC;
        }
        if (is_complete) {
            check(eos_remain < received, "refund eos must less then received");
            asset eos_send(eos_remain, EOS_SYM);
            action(permission_level{_self, "active"_n},
                EOS_CONTRACT, "transfer"_n,
                make_tuple(_self, from, eos_send, string("diffraction refund"))
            ).send();
            remain.amount = 0;
            total_send.amount += sa;
            total_received.amount += (received - eos_remain);
            check(total_send.amount == SEND_LIMIT, "total send amount overflow"); 
            statetable.modify(stat, _self, [&](auto& row) {
                row.round_remain = remain;
                row.round = round;
                row.total_send = total_send;
                row.rate = rate;
                row.available = false;
                row.total_received = total_received;
            });
            return sa;
        }
        int64_t s = eos_remain * rate;
        sa += s;
        remain.amount -= s;
        total_send.amount += sa;
        total_received.amount += received; 
        check(total_send.amount <= SEND_LIMIT, "the amount to be sent exceeds the upper limit"); 
        statetable.modify(stat, _self, [&](auto& row) {
            row.round_remain = remain;
            row.round = round;
            row.total_send = total_send;
            row.total_received = total_received;
            row.rate = rate;
        });
        return sa;
    }
};
