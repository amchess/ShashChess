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
    explicit ChessDb(std::string endpoint);
    explicit ChessDb(Action action);
    explicit ChessDb(std::string endpoint, Action action);
    ~ChessDb() override = default;

    std::vector<std::pair<std::string, Analysis>> lookup(const Position& position) override;

    void set_action(Action action);
    void set_min_rank(int min_rank);

   protected:
    Action      action;
    std::string endpoint;
    int         min_rank = 2;

    std::pair<std::string, Analysis>
    parse_move(const Position& position, std::string& item, bool check_rank) const;

    std::vector<std::pair<std::string, Analysis>> parse_query_all(const Position& position);
    std::vector<std::pair<std::string, Analysis>> parse_query_best(const Position& position);

    [[nodiscard]] std::string format_uri(const Position& position) const;
};
}


#endif
#endif  //CHESS_DB_H
