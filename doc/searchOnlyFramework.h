//search.h only framework
#ifndef SEARCH_H_INCLUDED
#define SEARCH_H_INCLUDED
...
#include "types.h"
//from Shashin begin
#include "shashin/shashin_position.h"
#include "evaluate.h"
#include "book/book_manager.h"
#include <memory>
#include "shashin/shashin_manager.h"
//from Shashin end
namespace ShashChess {

class ShashinManager;  //shashin
...
namespace Search {
...
struct Stack {
	...    
	int                         quietMoveStreak;
    //from Shashin Crystal-shashin begin
    bool secondaryLine = false;
    bool mainLine      = false;
    //from Shashin Crystal-shashin end
};

...
// The UCI stores the uci options, thread pool, and transposition table.
// This struct is used to easily forward data to the Search::Worker class.
struct SharedState {
	...    
	const ShashChess::ShashinConfig                 shashinConfig; //Shashin 
};
...
// SearchManager manages the search from the main thread. It is responsible for
// keeping track of the time, and storing data strictly related to the main thread.
class SearchManager: public ISearchManager {
   public:
	...
    void pv(Search::Worker&           worker,
            const ThreadPool&         threads,
            const TranspositionTable& tt,
            Depth                     depth,
            bool                      updateShashin);  //shashin

    ShashChess::TimeManagement tm;
	...
};
...

class Worker {
   public:
	...    
	bool            nmpGuard = false, nmpGuardV = false, nmpSide = false;  //from Crystal-shashin
    ShashinManager& getShashinManager();                                   //from crystal-Shashin
    int             lastShashinUpdatedDepth = 0;

   private:
	...
    //Shashin begin
    std::unique_ptr<ShashinManager>  shashinManager;
    const ShashChess::ShashinConfig& shConfig;
    friend class ShashChess::ThreadPool;
    //Shashin end
    friend class SearchManager;
};
...
}  // namespace ShashChess

#endif  // #ifndef SEARCH_H_INCLUDED
