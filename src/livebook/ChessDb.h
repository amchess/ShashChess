#ifndef CHESS_DB_H
#define CHESS_DB_H
#ifdef USE_LIVEBOOK

    #include "BaseLivebook.h"

namespace ShashChess::Livebook {

enum class Action {
    QUERY_ALL,
    QUERY_BEST,
    QUERY,
    QUERY_SEARCH,
};

class ChessDb: public BaseLivebook {
   public:
    ChessDb();
    explicit ChessDb(std::string endpoint_);
    explicit ChessDb(Action action_);
    explicit ChessDb(std::string endpoint_, Action action_);
    ~ChessDb() override = default;

    std::vector<std::pair<std::string, Analysis>> lookup(const Position& position_) override;

    void set_action(Action action_);
    void set_min_rank(int min_rank_);

   protected:
    Action      action;
    std::string endpoint;
    int         min_rank = 2;

    std::pair<std::string, Analysis>
    parse_move(const Position& position_, std::string& item_, bool check_rank_) const;

    std::vector<std::pair<std::string, Analysis>> parse_query_all(const Position& position_);
    std::vector<std::pair<std::string, Analysis>> parse_query_best(const Position& position_);

    [[nodiscard]] std::string format_uri(const Position& position_) const;
};
}


#endif
#endif  //CHESS_DB_H
