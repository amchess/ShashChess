#ifdef USE_LIVEBOOK
    #include "LichessLivebook.h"

    #include "../uci.h"

using namespace ShashChess::Livebook;

std::vector<std::pair<std::string, Analysis>> LichessLivebook::lookup(const Position& position) {
    const std::string full_uri = format_url(position);
    auto              ret      = std::vector<std::pair<std::string, Analysis>>();

    if (const CURLcode res = do_request(full_uri); res != CURLE_OK)
    {
        return ret;
    }

    try
    {
        auto data = nlohmann::json::parse(readBuffer);

        if (!data.contains("moves") || !data["moves"].is_array())
        {
            std::cerr << "Error parsing JSON: \"moves\" not found" << std::endl
                      << readBuffer << std::endl;
            return ret;
        }

        auto moves = data["moves"];

        for (auto move : moves)
        {
            if (!move.contains("uci") || !move["uci"].is_string())
            {
                std::cerr << "Error parsing JSON: \"uci\" not found" << std::endl
                          << readBuffer << std::endl;
                continue;
            }

            auto uci_move = move["uci"].get<std::string>();

            if (const auto uci = UCIEngine::to_move(position, uci_move); !uci)
            {
                continue;
            }

            auto analysis = parse_analysis(move);

            if (analysis == nullptr)
            {
                continue;
            }

            if (position.side_to_move() == BLACK)
            {
                analysis = analysis->flip();
            }

            auto element = std::pair(uci_move, *analysis);
            ret.push_back(element);
        }
    } catch (const nlohmann::json::exception& e)
    {
        std::cerr << "Error parsing JSON: " << e.what() << std::endl << readBuffer << std::endl;
        return ret;
    }

    return ret;
}

#endif
