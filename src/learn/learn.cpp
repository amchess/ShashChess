/*
  ShashChess, a UCI chess playing engine derived from Stockfish
  Copyright (C) 2004-2026 The Stockfish developers (see AUTHORS file)
  Copyright (C) 2018-2026 The ShashChess developers (see AUTHORS file)

  ShashChess is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  ShashChess is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <iostream>
#include <fstream>
#include <sstream>
#include "../misc.h"
#include "learn.h"
#include <algorithm>
#include <cassert>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <system_error>
#include <thread>
#include <type_traits>
#include "../movegen.h"
#include "../score.h"
#include "../uci.h"

using namespace std;
using namespace ShashChess;

LearningData LD;

namespace {

// [SC:learning] begin
// The experience files of more engines running in the same folder (see learn.h)
namespace fs = std::filesystem;

constexpr char ExperienceName[]     = "experience.exp";
constexpr char ExperienceLockName[] = "experience.lock";

ExperienceFileStamp file_stamp(const string& filename) {
    ExperienceFileStamp stamp;
    std::error_code     ec;
    const fs::path      path(filename);

    const auto size = fs::file_size(path, ec);
    if (ec)
        return stamp;

    const auto time = fs::last_write_time(path, ec);
    if (ec)
        return stamp;

    stamp.exists = true;
    stamp.size   = size;
    stamp.time   = static_cast<long long>(time.time_since_epoch().count());
    return stamp;
}

// The stamp of the file just written in place of 'replaced'. Its time is made at least two
// seconds later than the time of the file it replaced: two engines may save a file of the
// same size within the resolution of the file times (up to two seconds), and the first one
// would not see that the file is no more the one it wrote
ExperienceFileStamp stamp_written_file(const string&              filename,
                                       const ExperienceFileStamp& replaced) {
    using FileDuration = fs::file_time_type::duration;

    ExperienceFileStamp stamp = file_stamp(filename);
    if (!stamp.exists || !replaced.exists)
        return stamp;

    const long long step = static_cast<long long>(
      std::chrono::duration_cast<FileDuration>(std::chrono::seconds(2)).count());
    if (stamp.time < replaced.time + step)
    {
        std::error_code ec;
        fs::last_write_time(
          fs::path(filename),
          fs::file_time_type(FileDuration(static_cast<FileDuration::rep>(replaced.time + step))),
          ec);
        stamp = file_stamp(filename);
    }
    return stamp;
}

// Only one engine at a time merges and replaces the experience files. The lock is a
// directory: creating it either succeeds or fails because it is there, on every file
// system, and it needs nothing outside the standard library.
// An engine never waits long: a GUI terminates the engine that does not answer in time
// (cutechess: 5 seconds after 'quit'), and an engine terminated while it has the lock
// leaves it behind. After MaxWait the engine goes on without the lock, and a lock older
// than StaleAfter is removed. Without the lock the files are still written whole (each
// engine has its own temporary file, renamed when complete): at worst the moves that
// another engine saves in that moment are missing until it saves again.
class ExperienceLock {
   public:
    ExperienceLock(const string& name, bool acquire) :
        path(name) {
        using Clock = std::chrono::steady_clock;

        static constexpr auto MaxWait    = std::chrono::seconds(2);
        static constexpr auto StaleAfter = std::chrono::seconds(15);
        static constexpr auto Pause      = std::chrono::milliseconds(10);

        if (!acquire)
            return;

        const auto start = Clock::now();
        while (true)
        {
            std::error_code ec;
            if (fs::create_directory(path, ec))
            {
                locked = true;
                return;
            }

            // Not because it is there: the folder cannot be written
            if (ec)
                return;

            const auto time = fs::last_write_time(path, ec);
            if (!ec)
            {
                const auto age = fs::file_time_type::clock::now() - time;
                if (age > StaleAfter || age < -StaleAfter)
                    fs::remove(path, ec);
            }

            if (Clock::now() - start > MaxWait)
                return;

            std::this_thread::sleep_for(Pause);
        }
    }

    ~ExperienceLock() {
        if (locked)
        {
            std::error_code ec;
            fs::remove(path, ec);
        }
    }

    ExperienceLock(const ExperienceLock&)            = delete;
    ExperienceLock& operator=(const ExperienceLock&) = delete;

    [[nodiscard]] bool owned() const { return locked; }

   private:
    fs::path path;
    bool     locked = false;
};

// Hexadecimal digits that no other running engine has: the process id and a random number.
// Before, the name of the Concurrent Experience file came from the clock alone, in
// milliseconds: two engines started together by cutechess could get the same file
const string& instance_id() {
    static const string id = [] {
#ifdef _WIN32
        const auto pid = static_cast<unsigned long long>(GetCurrentProcessId());
#else
        const auto pid = static_cast<unsigned long long>(getpid());
#endif
        PRNG prng((static_cast<uint64_t>(now()) ^ (static_cast<uint64_t>(pid) << 32)) | 1);

        stringstream ss;
        ss << hex << pid << setw(8) << setfill('0') << (prng.rand<uint64_t>() >> 32);
        return ss.str();
    }();

    return id;
}

bool is_hex(const string& s) {
    return !s.empty() && std::all_of(s.begin(), s.end(), [](unsigned char c) {
        return std::isxdigit(c) != 0;
    });
}

bool is_number(const string& s) {
    return !s.empty() && std::all_of(s.begin(), s.end(), [](unsigned char c) {
        return std::isdigit(c) != 0;
    });
}

struct SlaveFile {
    string    path;
    bool      temporary = false;
    long long time      = 0;
};

// The other experience files in the folder of the experience file, oldest first:
// - experience<number>.exp: slave files, as before (now also with gaps in the numbers)
// - experience-<hex>.exp: the file of an engine with Concurrent Experience. Before, these
//   were never read again: what was learned with that option was written and then lost
// - experience_new.exp, experience_new-<hex>.exp: files left by an engine terminated while
//   saving, or that could not be renamed. Only with recoverTemporaries (the caller has the
//   lock) and if not just written: it may be the file another engine is saving right now
// Any other name (a backup copy, for instance) is left alone.
vector<SlaveFile> find_slave_files(bool recoverTemporaries) {
    static constexpr auto TemporaryMinAge = std::chrono::seconds(10);

    const string Prefix = "experience";
    const string Suffix = ".exp";

    vector<SlaveFile> files;
    std::error_code   ec;

    fs::path folder = fs::path(Util::map_path(ExperienceName)).parent_path();
    if (folder.empty())
        folder = fs::path(".");

    for (fs::directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec))
    {
        // A name with characters outside ASCII is not ours (and cannot always be
        // converted to a string)
        const fs::path filename = it->path().filename();
        string         name;
        bool           ascii = true;
        for (const auto c : filename.native())
        {
            const auto u = static_cast<unsigned long>(
              static_cast<std::make_unsigned_t<fs::path::value_type>>(c));
            if (u == 0 || u > 127)
            {
                ascii = false;
                break;
            }
            name += static_cast<char>(u);
        }

        if (!ascii || name.size() <= Prefix.size() + Suffix.size()
            || name.compare(0, Prefix.size(), Prefix) != 0
            || name.compare(name.size() - Suffix.size(), Suffix.size(), Suffix) != 0)
            continue;

        const string mid =
          name.substr(Prefix.size(), name.size() - Prefix.size() - Suffix.size());

        SlaveFile file;
        if (mid == "_new" || (mid.compare(0, 5, "_new-") == 0 && is_hex(mid.substr(5))))
            file.temporary = true;
        else if (!is_number(mid) && !(mid[0] == '-' && is_hex(mid.substr(1))))
            continue;

        std::error_code fileEc;
        if (!it->is_regular_file(fileEc) || fileEc)
            continue;

        const auto time = fs::last_write_time(it->path(), fileEc);
        if (fileEc)
            continue;

        if (file.temporary)
        {
            const auto age = fs::file_time_type::clock::now() - time;
            if (!recoverTemporaries || (age < TemporaryMinAge && age > -TemporaryMinAge))
                continue;
        }

        file.path = Util::map_path(name);
        file.time = static_cast<long long>(time.time_since_epoch().count());
        files.push_back(file);
    }

    // Oldest first: for a move stored in two files with the same depth, the newest prevails
    std::stable_sort(files.begin(), files.end(),
                     [](const SlaveFile& a, const SlaveFile& b) { return a.time < b.time; });

    return files;
}

// Puts the complete new file in the place of the old one, in one step: there is always a
// whole experience file (before, the old one was removed and then the new one renamed:
// two engines saving together could remove each other's file). On Windows the rename fails
// while another program has one of the two files open (an engine loading the experience,
// an antivirus): try again for a second
bool replace_file(const string& from, const string& to) {
    const fs::path source(from), target(to);

    for (int attempt = 0; attempt < 20; ++attempt)
    {
        std::error_code ec;
        fs::rename(source, target, ec);
        if (!ec)
            return true;

        // Old run-time libraries (MinGW before GCC 9) cannot rename onto an existing file
        if (attempt >= 2)
        {
            fs::remove(target, ec);
            fs::rename(source, target, ec);
            if (!ec)
                return true;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    return false;
}
// [SC:learning] end

// [SC:learning] Case insensitive, as the check of the combo values (Option::operator=): before,
// "setoption name Persisted learning value standard" was accepted and then taken for Self
bool is_mode(const string& lm, const char* mode) {
    return !CaseInsensitiveLess()(lm, mode) && !CaseInsensitiveLess()(mode, lm);
}

LearningMode identify_learning_mode(const string& lm) {
    if (is_mode(lm, "Standard"))
        return LearningMode::Standard;

    if (is_mode(lm, "Self"))
        return LearningMode::Self;

    return LearningMode::Off;
}
}

bool LearningData::load(const string& filename, bool allowPartial) {
    ifstream in(filename, ios::in | ios::binary);

    //Quick exit if file is not present
    if (!in.is_open())
        return false;

    //Get experience file size
    in.seekg(0, ios::end);
    size_t fileSize = in.tellg();

    //File size should be a multiple of 'PersistedLearningMove'
    if (fileSize % sizeof(PersistedLearningMove))
    {
        if (!allowPartial)
        {
            cerr << "info string The file <" << filename << "> with size <" << fileSize
                 << "> is not a valid experience file" << endl;
            return false;
        }

        // [SC:learning] The engine was terminated while writing it: the complete moves
        fileSize -= fileSize % sizeof(PersistedLearningMove);
    }

    //An empty file is a valid experience file without moves (persist() writes it when no move
    //was searched): nothing to read. The loop below would otherwise read past the buffer
    if (fileSize == 0)
        return true;

    //Allocate buffer to read the entire file
    void* fileData = malloc(fileSize);
    if (!fileData)
    {
        cerr << "info string Failed to allocate <" << fileSize
             << "> bytes to read experience file <" << filename << ">" << endl;
        return false;
    }

    //Read the entire file
    in.seekg(0, ios::beg);  //Move read pointer to the beginning of the file
    in.read(static_cast<char*>(fileData), static_cast<std::streamsize>(fileSize));
    if (!in)
    {
        free(fileData);

        cerr << "info string Failed to read <" << fileSize << "> bytes from experience file <"
             << filename << ">" << endl;
        return false;
    }

    //Close the input data file
    in.close();

    //Save pointer to fileData to be freed later
    mainDataBuffers.push_back(fileData);

    //Loop the moves from this file
    const bool qLearning             = learningMode == LearningMode::Self;
    auto*      persistedLearningMove = static_cast<PersistedLearningMove*>(fileData);
    do
    {
        insert_or_update(persistedLearningMove, qLearning, true);
        ++persistedLearningMove;
    } while (reinterpret_cast<size_t>(persistedLearningMove)
             < reinterpret_cast<size_t>(fileData) + fileSize);

    return true;
}

// [SC:learning] begin
// The experience file was written by another engine since this one read or wrote it: its
// moves go into the table, so that saving the table does not delete them. Unlike load(),
// the file is read a block at a time and only the moves that are new for the table are
// kept in memory: this runs at every save, and load() keeps the whole file
bool LearningData::merge(const string& filename) {
    static constexpr size_t BlockMoves = 4096;

    ifstream in(filename, ios::in | ios::binary);
    if (!in.is_open())
        return false;

    const bool                    qLearning = learningMode == LearningMode::Self;
    vector<PersistedLearningMove> block(BlockMoves);
    PersistedLearningMove*        newPlm = nullptr;

    while (in)
    {
        in.read(reinterpret_cast<char*>(block.data()),
                static_cast<std::streamsize>(block.size() * sizeof(PersistedLearningMove)));

        const size_t moves = static_cast<size_t>(in.gcount()) / sizeof(PersistedLearningMove);
        for (size_t i = 0; i < moves; ++i)
        {
            if (!newPlm)
            {
                newPlm =
                  static_cast<PersistedLearningMove*>(malloc(sizeof(PersistedLearningMove)));
                if (!newPlm)
                {
                    cerr << "info string Failed to allocate <" << sizeof(PersistedLearningMove)
                         << "> bytes to merge experience file <" << filename << ">" << endl;
                    return false;
                }
            }

            newPlm->key          = block[i].key;
            newPlm->learningMove = block[i].learningMove;

            // If the table keeps the pointer, the next move needs a new buffer
            if (insert_or_update(newPlm, qLearning, true))
            {
                newMovesDataBuffers.push_back(newPlm);
                newPlm = nullptr;
            }
        }
    }

    free(newPlm);
    return true;
}

bool LearningData::is_dirty(const PersistedLearningMove& plm) const {
    return dirtyAll || dirtyMoves.count({plm.key, plm.learningMove.move.raw()}) != 0;
}
// [SC:learning] end

inline bool should_update(const LearningMove existing_move, const LearningMove learning_move) {
    if (learning_move.depth > existing_move.depth)
    {
        return true;
    }

    if (learning_move.depth < existing_move.depth)
    {
        return false;
    }

    if (learning_move.score != existing_move.score)
    {
        return true;
    }

    return learning_move.performance != existing_move.performance;
}

// Returns true if the hash table keeps a pointer to plm->learningMove, false if plm was only
// used to update a move already in the table (and then the caller may free plm)
bool LearningData::insert_or_update(PersistedLearningMove* plm, bool qLearning, bool fromDisk) {
    // We search in the range of all the hash table entries with plm key
    const auto [first, second] = HT.equal_range(plm->key);

    // [SC:learning] What this engine learns is what it has to add to the experience file
    // when it saves (see persist())
    const auto learned = [this, plm, fromDisk]() {
        if (!fromDisk)
            dirtyMoves.insert({plm->key, plm->learningMove.move.raw()});
    };

    //If the plm key belongs to a position that did not exist before in the 'LHT'
    //then, we insert this new key and LearningMove and return
    if (first == second)
    {
        //Insert new key and learningMove
        HT.insert({plm->key, &plm->learningMove});
        learned();

        //Flag for persisting
        needPersisting = true;

        //Nothing else to do
        return true;
    }

    //The plm key belongs to a position already existing in the LHT
    //Check if this move already exists for this position
    const auto itr = find_if(
      first, second, [&plm](const auto& p) { return p.second->move == plm->learningMove.move; });

    //If the move does not exist then insert it
    LearningMove* bestNewMoveCandidate = nullptr;
    bool          inserted             = false;
    if (itr == second)
    {
        HT.insert({plm->key, &plm->learningMove});
        learned();
        bestNewMoveCandidate = &plm->learningMove;
        inserted             = true;

        //Flag for persisting
        needPersisting = true;
    }
    else  //If the move exists, check if it better than the move we already have
    {
        LearningMove* existingMove = itr->second;

        // [SC:learning] A move learned by this engine since its last save is newer than
        // the one in the file, written by another engine: the file prevails only if deeper
        if (fromDisk && plm->learningMove.depth <= existingMove->depth && is_dirty(*plm))
            return false;

        if (should_update(*existingMove, plm->learningMove))
        {
            //Replace the existing move
            *existingMove = plm->learningMove;
            learned();

            //Since an existing move was replaced, check the best move again
            bestNewMoveCandidate = existingMove;

            //Flag for persisting
            needPersisting = true;
        }
    }

    //Do we have a candidate for new best move?
    if (bestNewMoveCandidate != nullptr)
    {
        bool          newBestMove     = false;
        LearningMove* currentBestMove = first->second;
        if (bestNewMoveCandidate != currentBestMove)
        {
            if (qLearning)
            {
                if (bestNewMoveCandidate->score > currentBestMove->score)
                {
                    newBestMove = true;
                }
            }
            else
            {
                if ((currentBestMove->depth < bestNewMoveCandidate->depth)
                    || (currentBestMove->depth == bestNewMoveCandidate->depth
                        && currentBestMove->score <= bestNewMoveCandidate->score))
                {
                    newBestMove = true;
                }
            }
        }

        if (newBestMove)
        {
            //Boring and slow, but I can't think of an alternative at the moment
            //This is not thread safe, but it is fine since this code will never be called from multiple threads
            static LearningMove lm;

            lm                    = *bestNewMoveCandidate;
            *bestNewMoveCandidate = *currentBestMove;
            *currentBestMove      = lm;

            //Flag for persisting
            needPersisting = true;
        }
    }

    return inserted;
}

LearningData::LearningData() :
    isPaused(false),
    isReadOnly(false),
    needPersisting(false),
    isSuspended(false),
    learningMode(LearningMode::Off),
    pausedKey(0),
    dirtyAll(false),
    masterSynced(false) {}

LearningData::~LearningData() { clear(); }

void LearningData::clear() {
    //Clear hash table
    HT.clear();

    //Release internal data buffers
    for (void* p : mainDataBuffers)
        free(p);

    //Clear internal data buffers
    mainDataBuffers.clear();

    //Release internal new moves data buffers
    for (void* p : newMovesDataBuffers)
        free(p);

    //Clear internal new moves data buffers
    newMovesDataBuffers.clear();

    //Nothing left to persist
    needPersisting = false;

    // [SC:learning] and nothing read from the experience file
    dirtyMoves.clear();
    dirtyAll     = false;
    masterSynced = false;
}

void LearningData::init(ShashChess::OptionsMap& o) {
    OptionsMap& options = o;

    //init() is also called when Persisted learning or Experience Book change: save what was
    //learned since the last save, otherwise clear() would discard it
    if (needPersisting && !isReadOnly)
        persist(options);

    clear();

    // [SC:learning] A new learning mode (or the experience reloaded) starts afresh: before, a
    // pause stayed even switching Persisted learning Off and on again
    resume();

    learningMode = identify_learning_mode(options["Persisted learning"]);
    if ((learningMode == LearningMode::Off) && !((bool) options["Experience Book"]))
        return;

    // [SC:learning] begin
    // The other engines running in this folder do not save, and this one does not read
    // their files, while the experience is consolidated here. In read-only mode nothing is
    // written or removed, and no lock is needed
    const ExperienceLock lock(Util::map_path(ExperienceLockName), !isReadOnly);

    //Slave experience files (if any): the numbered ones, those of the engines with
    //Concurrent Experience and, just in case, the temporary files which will be present if
    //a previous saving operation failed (engine crashed or terminated)
    //The same path that persist() writes: the engine folder, not the working directory
    const vector<SlaveFile> candidates = find_slave_files(lock.owned());

    //The stamp is taken before reading: if the file is replaced while it is read here
    //(only without the lock), the next save finds a different stamp and merges it
    const string              experienceFilename = Util::map_path(ExperienceName);
    const ExperienceFileStamp stamp              = file_stamp(experienceFilename);
    if (load(experienceFilename))
    {
        masterStamp  = stamp;
        masterSynced = stamp.exists;
    }

    vector<string> slaveFiles;
    for (const SlaveFile& candidate : candidates)
        if (load(candidate.path, candidate.temporary))
            slaveFiles.push_back(candidate.path);
    // [SC:learning] end

    //We need to write all consolidated experience to disk
    //[SC:learning] and only then remove the slave files: before, they were removed also when
    //the save failed (for instance the rename, on Windows when an antivirus holds the new
    //file): experience_new.exp, then the only copy of the experience, was deleted. And not in
    //read-only mode, where nothing is written (before, there the slave files were just deleted)
    //Nor without the lock: another engine may be replacing the experience file with its own
    //table, without the moves of the slave files (they are consolidated at the next start)
    if (!slaveFiles.empty() && !isReadOnly && save(options, true) && lock.owned())
    {
        //Remove slave files
        for (const string& fn : slaveFiles)
            remove(fn.c_str());
    }

    // Clear the 'needPersisting' flag
    needPersisting = false;
}

void LearningData::quick_reset_exp() {
    const std::string experienceFilename = Util::map_path("experience.exp");
    std::cout << "Loading experience file: " << experienceFilename << std::endl;

    std::ifstream file(experienceFilename, std::ifstream::binary | std::ifstream::ate);
    if (!file)
    {
        std::cerr << "Failed to load experience file" << std::endl;
        return;
    }

    const std::streamsize  file_size     = file.tellg();
    constexpr unsigned int entry_size    = 24;
    const unsigned int     total_entries = file_size / entry_size;

    file.close();

    std::cout << "Total entries in the file: " << total_entries << std::endl;

    if (const auto check = load(experienceFilename); !check)
    {
        std::cerr << "Failed to load experience file" << std::endl;
        return;
    }

    std::cout << "Successfully loaded experience file" << std::endl;

    int entry_count = 0;
    for (auto& [key, learning_move] : HT)
    {
        entry_count++;

        const auto old_performance = learning_move->performance;
        const auto new_performance =
          WDLModel::get_win_probability(learning_move->score, learning_move->depth);

        std::cout << "Updating entry " << entry_count << "/" << total_entries << " Key " << key
                  << "Value " << learning_move->score << "Depth " << learning_move->depth
                  << ": old performance=" << static_cast<int>(old_performance)
                  << ", new performance=" << static_cast<int>(new_performance) << std::endl;

        learning_move->performance = new_performance;
    }

    needPersisting = true;
    dirtyAll       = true;  // [SC:learning] Every move was changed here, not in the file
    std::cout << "Finished updating performances. Total processed entries: " << entry_count
              << std::endl;
}


void LearningData::set_learning_mode(ShashChess::OptionsMap& options, const string& lm) {
    LearningMode newLearningMode = identify_learning_mode(lm);
    if (newLearningMode == learningMode)
        return;

    init(options);
}

LearningMode LearningData::learning_mode() const { return learningMode; }

bool LearningData::persist(const ShashChess::OptionsMap& o) {
    //Quick exit if we have nothing to persist
    if (HT.empty() || !needPersisting)
        return true;

    if (isReadOnly)
    {
        //We should not be here if we are running in ReadOnly mode
        assert(false);
        return false;
    }

    // [SC:learning] One engine at a time: cutechess sends 'ucinewgame' and 'quit' to the
    // two engines of a game in the same moment, and both save
    const ExperienceLock lock(Util::map_path(ExperienceLockName), true);

    return save(o, false);
}

bool LearningData::save(const ShashChess::OptionsMap& o, bool consolidate) {
    const OptionsMap& options = o;
    //Quick exit if we have nothing to persist
    if (HT.empty() || !needPersisting)
        return true;

    if (isReadOnly)
    {
        //We should not be here if we are running in ReadOnly mode
        assert(false);
        return false;
    }

    /*
        To avoid any problems when saving to experience file, we will actually do the following:
        1) Merge "experience.exp" into the table, if another engine has written it
        2) Save new experience to "experience_new-<id>.exp", a file of this engine only
        3) Rename it to "experience.exp", replacing the old file in one step

        This approach is fail proof so that the old file is only replaced when the new file is successfully saved!
        If, for whatever odd reason, the engine is able to execute step (2) and fails to execute step (3)
        i.e., we end up with experience_new-<id>.exp then it is not a problem since the file will be loaded anyway the next
        time the engine starts!

        [SC:learning] Before, every engine wrote the same "experience_new.exp", and removed
        "experience.exp" before renaming: with two engines saving together the file was a mix of
        the two tables, or the rename of one failed and the remove of the other deleted the only file.
        And each engine wrote only its own table: the last to save deleted what the others had learned.
    */

    // With Concurrent Experience each engine writes a file of its own and does not read the
    // experience file again: the files are consolidated when an engine starts (see init())
    const bool ownFile = !consolidate && static_cast<bool>(options["Concurrent Experience"]);

    const string experienceFilename =
      Util::map_path(ownFile ? "experience-" + instance_id() + ".exp" : ExperienceName);
    const string tempExperienceFilename =
      Util::map_path("experience_new-" + instance_id() + ".exp");

    ExperienceFileStamp replaced;
    if (!ownFile)
    {
        replaced = file_stamp(experienceFilename);
        if (replaced.exists && !(masterSynced && replaced == masterStamp))
            merge(experienceFilename);
    }

    ofstream outputFile(tempExperienceFilename, ofstream::trunc | ofstream::binary);
    if (!outputFile.is_open())
    {
        sync_cout << "info string Cannot create <" << tempExperienceFilename
                  << ">: the experience is not saved" << sync_endl;
        return false;
    }

    //Zeroed once and then filled field by field: the padding bytes written to the file are
    //always 0 (before, they were whatever was in memory)
    PersistedLearningMove persistedLearningMove;
    std::memset(static_cast<void*>(&persistedLearningMove), 0, sizeof(persistedLearningMove));
    for (auto& kvp : HT)
    {
        const LearningMove& learningMove = *kvp.second;
        if (learningMove.depth != 0)
        {
            persistedLearningMove.key                      = kvp.first;
            persistedLearningMove.learningMove.depth       = learningMove.depth;
            persistedLearningMove.learningMove.score       = learningMove.score;
            persistedLearningMove.learningMove.move        = learningMove.move;
            persistedLearningMove.learningMove.performance = learningMove.performance;
            outputFile.write(reinterpret_cast<char*>(&persistedLearningMove),
                             sizeof(persistedLearningMove));
        }
    }
    outputFile.close();

    //Replace the experience file only with a complete new one: if a write failed (disk full,
    //I/O error) the old file must stay where it is
    if (outputFile.fail())
    {
        remove(tempExperienceFilename.c_str());
        sync_cout << "info string Cannot write <" << tempExperienceFilename
                  << ">: the experience is not saved, <" << experienceFilename << "> is unchanged"
                  << sync_endl;
        return false;
    }

    // [SC:learning] If the rename fails, the next save tries again; and if there is none,
    // the next start loads the temporary file as a slave file
    if (!replace_file(tempExperienceFilename, experienceFilename))
    {
        sync_cout << "info string Cannot rename <" << tempExperienceFilename << "> to <"
                  << experienceFilename << ">: the experience is saved in <"
                  << tempExperienceFilename << ">" << sync_endl;
        return false;
    }

    // [SC:learning] The file is now this table: nothing to merge until another engine
    // writes it. Without a stamp different from that of the file replaced, the next save
    // merges it anyway
    if (!ownFile)
    {
        masterStamp  = stamp_written_file(experienceFilename, replaced);
        masterSynced = masterStamp.exists && !(replaced.exists && masterStamp == replaced);
    }
    dirtyMoves.clear();
    dirtyAll = false;

    //Prevent persisting again without modifications
    needPersisting = false;

    return true;
}

void LearningData::pause(Key decidedKey) {
    isPaused  = true;
    pausedKey = decidedKey;
}

void LearningData::resume() { isPaused = false; }

void LearningData::add_new_learning(Key key, const LearningMove& lm) {
    //Allocate buffer to read the entire file
    auto* newPlm = static_cast<PersistedLearningMove*>(malloc(sizeof(PersistedLearningMove)));
    if (!newPlm)
    {
        cerr << "info string Failed to allocate <" << sizeof(PersistedLearningMove)
             << "> bytes for new learning entry" << endl;
        return;
    }

    //Save pointer to fileData to be freed later
    newMovesDataBuffers.push_back(newPlm);

    //Assign
    newPlm->key          = key;
    newPlm->learningMove = lm;

    //Add to HT. If the move was already there, it has only been updated with newPlm: free the
    //buffer at once, otherwise each 'position ... moves' of a game would allocate again all
    //its moves until the engine is closed
    if (!insert_or_update(newPlm, learningMode == LearningMode::Self, false))
    {
        newMovesDataBuffers.pop_back();
        free(newPlm);
    }
}

int LearningData::probeByMaxDepthAndScore(Key key, const LearningMove*& learningMove) {
    const LearningMove* maxDepthMove = nullptr;
    int                 maxDepth     = -1;
    int                 maxScore     = -1;

    // Iterate through the range of elements with the given key
    auto range = HT.equal_range(key);
    if (range.first == range.second)
    {
        learningMove = nullptr;
        return 0;
    }
    const auto siblings = distance(range.first, range.second);
    for (auto it = range.first; it != range.second; ++it)
    {
        LearningMove* move = it->second;

        // Check if the current move has a greater depth than the maximum depth found so far
        if (move->depth > maxDepth)
        {
            maxDepth     = move->depth;
            maxScore     = move->score;
            maxDepthMove = move;
        }
        // If the current move has the same depth as the maximum depth found so far,
        // check if it has a greater score
        else if (move->depth == maxDepth && move->score > maxScore)
        {
            maxScore     = move->score;
            maxDepthMove = move;
        }
    }

    // Return the reference to the LearningMove with the maximum depth and score (or nullptr if not found)
    learningMove = maxDepthMove;

    return static_cast<int>(siblings);
}

const LearningMove* LearningData::probe_move(Key key, Move move) {
    auto range = HT.equal_range(key);

    if (range.first == range.second)
        return nullptr;

    auto itr =
      find_if(range.first, range.second, [&move](const auto& p) { return p.second->move == move; });

    if (itr == range.second)
        return nullptr;

    return itr->second;
}


void LearningData::sortLearningMoves(std::vector<LearningMove*>& learningMoves) {
    std::sort(learningMoves.begin(), learningMoves.end(),
              [](const LearningMove* a, const LearningMove* b) {
                  if (a->depth != b->depth)
                  {
                      return a->depth > b->depth;
                  }
                  const int winProbA = a->performance;
                  const int winProbB = b->performance;

                  if (winProbA != winProbB)
                  {
                      return winProbA > winProbB;
                  }
                  return a->score > b->score;
              });
}
vector<LearningMove*> LearningData::probe(ShashChess::Key key) {
    vector<LearningMove*> result;
    auto                  range = HT.equal_range(key);
    for (auto it = range.first; it != range.second; ++it)
    {
        result.push_back(it->second);
    }

    return result;
}
// [SC:learning] "Show Experience" button (proposed by ALF): the same data as show_exp(), but
// as "info string" lines, so that a GUI shows it (Arena, Shredder; the ChessBase GUIs only
// write them in the Debug Log File). Moves are listed in the order the Experience Book picks by:
// depth, then win probability, then score
void LearningData::show_exp_info(const Position& pos, bool experienceBook) {
    const string experienceFilename = Util::map_path("experience.exp");
    const bool   found              = ifstream(experienceFilename, ios::in | ios::binary).is_open();
    sync_cout << "info string Experience file: " << experienceFilename
              << (found ? "" : " (not found)") << ", " << LD.HT.size() << " entries in memory"
              << sync_endl;
    sync_cout << "info string Experience for the current position: " << pos.fen() << sync_endl;

    if (!LD.is_enabled())
    {
        if (!experienceBook)
        {
            sync_cout << "info string Persisted learning and Experience Book are Off: the "
                         "experience file is not loaded"
                      << sync_endl;
            return;
        }

        sync_cout << "info string Persisted learning is Off: the table is only used by the "
                     "Experience Book (nothing is learned or written)"
                  << sync_endl;
    }
    else if (LD.is_paused())
        sync_cout << "info string Learning is paused: the game was found decided and its "
                     "experience saved. It resumes with ucinewgame or a position that does "
                     "not continue that game"
                  << sync_endl;

    // A different position may have the same key: only the moves legal here
    vector<LearningMove*> learningMoves = LD.probe(pos.key());
    const MoveList<LEGAL> legalMoves(pos);
    learningMoves.erase(std::remove_if(learningMoves.begin(), learningMoves.end(),
                                       [&legalMoves](const LearningMove* lm) {
                                           return !legalMoves.contains(lm->move);
                                       }),
                        learningMoves.end());

    if (learningMoves.empty())
    {
        sync_cout << "info string   no experience data for this position" << sync_endl;
        return;
    }

    sortLearningMoves(learningMoves);
    for (const LearningMove* lm : learningMoves)
    {
        sync_cout << "info string   " << UCIEngine::move(lm->move, pos.is_chess960());

        // A move of the game in progress, stored without a search: it is never saved
        if (lm->depth <= 0)
            cout << " played, not searched";
        else
        {
            // The value is in internal units, as in the file; in brackets as in the analysis
            cout << " depth " << lm->depth << " value " << lm->score;
            if (lm->score > -VALUE_INFINITE && lm->score < VALUE_INFINITE)
                cout << " (" << UCIEngine::format_score(Score(lm->score, pos)) << ")";
            cout << " win probability " << lm->performance << "%";
        }
        cout << sync_endl;
    }
}

void LearningData::show_exp(const Position& pos) {
    sync_cout << pos << endl;
    cout << "Experience: ";
    vector<LearningMove*> learningMoves = LD.probe(pos.key());
    if (learningMoves.empty())
    {
        cout << "No experience data found for this position" << sync_endl;
        return;
    }

    sortLearningMoves(learningMoves);

    cout << endl;
    for (const auto& move : learningMoves)
    {
        const int winProb = move->performance;
        cout << "move: " << UCIEngine::move(move->move, pos.is_chess960())
             << " depth: " << move->depth << " value: " << move->score
             << " win probability: " << winProb << endl;
    }
    cout << sync_endl;
}
