/*
  Alexander, a UCI chess playing engine derived from Stockfish
  Copyright (C) 2004-2025 The Alexander developers (see AUTHORS file)

  Alexander is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Alexander is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "evaluate_handicap.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include "../misc.h"
#include "../position.h"
#include "../types.h"
#include "../uci.h"
#include "../ucioption.h"
#include "../bitboard.h"
#include <string>
#include <ctime>
#include "../movegen.h"
#include "../shashin/shashin_types.h"
#include "../shashin/shashin_manager.h"
namespace Alexander {

namespace Eval {
bool limitStrength, pawnsToEvaluate, winnableToEvaluate, imbalancesToEvaluate,
  simulateHumanBlunders, handicappedDepth;
int uciElo, RandomEvalPerturb = 0;

// Thread-local random number generator
static thread_local std::mt19937_64 tls_rng(std::time(0));
MinMax                              min_max_threshold[WDLModel::MAX_WIN_PROBABILITY + 1];
uint8_t                             get_handicap_max_win_probability(uint8_t wp) {
    if (wp >= MIDDLE_HIGH_TAL_MAX + 1)
        return MIDDLE_TAL_MAX;  // From High Tal to Middle Tal (-2)
    else if (wp >= MIDDLE_TAL_MAX + 1)
        return MIDDLE_LOW_TAL_MAX;  // From High-Middle Tal to Middle-Low Tal (-2)
    else if (wp >= MIDDLE_LOW_TAL_MAX + 1)
        return LOW_TAL_MAX;  // From Middle Tal to Low Tal (-2)
    else if (wp >= LOW_TAL_MAX + 1)
        return CAPABLANCA_TAL_MAX;  // From Middle-Low Tal to Capablanca-Tal (-2)
    else if (wp >= CAPABLANCA_TAL_MAX + 1)
        return CAPABLANCA_MAX;  // From Low Tal to Capablanca (-2)
    else if (wp >= CAPABLANCA_MAX + 1)
        return CAPABLANCA_PETROSIAN_MAX;  // From Low Tal to Capablanca (-2)
    else if (wp >= CAPABLANCA_MAX)
        return LOW_PETROSIAN_MAX;  // From Capablanca to Low Petrosian (-2)
    else if (wp >= LOW_PETROSIAN_MAX + 1)
        return MIDDLE_LOW_PETROSIAN_MAX;  // From Capablanca-Petrosian to Middle-Low Petrosian (-2)
    else if (wp >= MIDDLE_LOW_PETROSIAN_MAX + 1)
        return MIDDLE_PETROSIAN_MAX;  // From Low Petrosian to Middle Petrosian (-2)
    else if (wp >= MIDDLE_PETROSIAN_MAX + 1)
        return MIDDLE_HIGH_PETROSIAN_MAX;  // From Middle-Low Petrosian to High-Middle Petrosian (-2)
    else
        return HIGH_PETROSIAN_MAX;  // eccezione: tutto il resto wp >=0
}

uint8_t get_handicap_min_win_probability(uint8_t wp) {
    if (wp <= HIGH_PETROSIAN_MAX)
        return MIDDLE_HIGH_PETROSIAN_MAX + 1;  // From High Petrosian to Middle Petrosian (+2)
    else if (wp <= MIDDLE_HIGH_PETROSIAN_MAX)
        return MIDDLE_PETROSIAN_MAX + 1;  // From High-Middle Petrosian to Middle-Low Petrosian (+2)
    else if (wp <= MIDDLE_PETROSIAN_MAX)
        return MIDDLE_LOW_PETROSIAN_MAX + 1;  // From Middle Petrosian to Low Petrosian (+2)
    else if (wp <= MIDDLE_LOW_PETROSIAN_MAX)
        return LOW_PETROSIAN_MAX + 1;  // From Middle-Low Petrosian to Capablanca-Petrosian (+2)
    else if (wp <= LOW_PETROSIAN_MAX)
        return CAPABLANCA_MAX;  // From Low Petrosian to Capablanca (+2)
    else if (wp <= CAPABLANCA_PETROSIAN_MAX)
        return CAPABLANCA_MAX + 1;  // From Low Petrosian to Capablanca (+2)
    else if (wp <= CAPABLANCA_MAX)
        return CAPABLANCA_TAL_MAX + 1;  // From Capablanca-Petrosian to Low Tal (+2)
    else if (wp <= CAPABLANCA_TAL_MAX)
        return LOW_TAL_MAX + 1;  // From Capablanca-Tal to Middle-Low Tal (+2)
    else if (wp <= LOW_TAL_MAX)
        return MIDDLE_LOW_TAL_MAX + 1;  // From Low Tal to Middle Tal (+2)
    else if (wp <= MIDDLE_LOW_TAL_MAX)
        return MIDDLE_TAL_MAX + 1;  // From Middle-Low Tal to High-Middle Tal (+2)
    else
        return MIDDLE_HIGH_TAL_MAX + 1;  // tutto il resto: wp <=100
}


void initHandicapMinMaxValueThresholds() {
    for (int wp = 0; wp <= WDLModel::MAX_WIN_PROBABILITY; ++wp)
    {
        int     minValue = 4000;   // Partiamo dal valore massimo
        int     maxValue = -4000;  // Partiamo dal valore minimo
        uint8_t max_wp   = get_handicap_max_win_probability(wp);
        uint8_t min_wp   = get_handicap_min_win_probability(wp);

        for (int materialClamp = 17; materialClamp <= 78; ++materialClamp)
        {
            for (int valueClamp = 4000; valueClamp >= -4000; --valueClamp)
            {
                if (WDLModel::get_win_probability_by_material(valueClamp, materialClamp) == max_wp)
                {
                    if (valueClamp > maxValue)
                    {
                        maxValue = valueClamp;
                    }
                }
                if (WDLModel::get_win_probability_by_material(valueClamp, materialClamp) == min_wp)
                {
                    if (valueClamp < minValue)
                    {
                        minValue = valueClamp;
                    }
                }
            }
        }

        min_max_threshold[wp] = {minValue, maxValue};
    }
}
Value get_handicap_value(Value baseEvaluation, int uciElo, const Position& pos) {
    // Determina la complessità della posizione
    bool complexPosition = isComplex(pos);

    // Configurazione della magnitudine degli errori per fascia Elo
    struct HandicapConfig {
        int minErrorMagnitude;  // Magnitudine minima dell'errore
        int maxErrorMagnitude;  // Magnitudine massima dell'errore
    };

    HandicapConfig handicapConfigs[] = {
      {MIN_ERROR_MAGNITUDE_BEGINNER, MAX_ERROR_MAGNITUDE_BEGINNER},  // Beginner: errori grandi
      {MIN_ERROR_MAGNITUDE_INTERMEDIATE,
       MAX_ERROR_MAGNITUDE_INTERMEDIATE},                            // Intermediate: errori medi
      {MIN_ERROR_MAGNITUDE_ADVANCED, MAX_ERROR_MAGNITUDE_ADVANCED},  // Advanced: errori piccoli
      {MIN_ERROR_MAGNITUDE_EXPERT, MAX_ERROR_MAGNITUDE_EXPERT}       // Expert: errori molto piccoli
    };

    HandicapConfig selectedConfig;

    // Determina la configurazione in base alla fascia Elo
    if (uciElo <= BEGINNER_MAX_ELO)
    {
        selectedConfig = handicapConfigs[0];
    }
    else if (uciElo <= INTERMEDIATE_MAX_ELO)
    {
        selectedConfig = handicapConfigs[1];
    }
    else if (uciElo <= ADVANCED_MAX_ELO)
    {
        selectedConfig = handicapConfigs[2];
    }
    else
    {
        selectedConfig = handicapConfigs[3];
    }

    // Generatore casuale per la magnitudine degli errori
    std::uniform_int_distribution<> errorDis(selectedConfig.minErrorMagnitude,
                                             selectedConfig.maxErrorMagnitude);
    int                             errorMagnitude = errorDis(tls_rng);

    // Aggiustamento della magnitudine dell'errore basato sulla complessità della posizione
    if (complexPosition)
    {
        errorMagnitude = static_cast<int>(
          errorMagnitude
          * COMPLEX_POSITION_MULTIPLIER);  // Aumenta l'errore per posizioni complesse
    }

    // Introduci un bias basato sulla fase della partita
    int gamePly = pos.game_ply();
    if (gamePly < OPENING_PHASE_MAX_PLY)
    {
        // Apertura: errori più casuali e variabili
        std::uniform_int_distribution<> openingDis(-errorMagnitude / 2, errorMagnitude / 2);
        errorMagnitude += openingDis(tls_rng);
    }
    else if (gamePly <= MIDDLEGAME_PHASE_MAX_PLY)
    {
        // Mediogioco: errori più strategici
        std::uniform_int_distribution<> middlegameDis(-errorMagnitude / 3, errorMagnitude / 3);
        errorMagnitude += middlegameDis(tls_rng);
    }
    else
    {
        // Finale: errori più posizionali
        std::uniform_int_distribution<> endgameDis(-errorMagnitude / 4, errorMagnitude / 4);
        errorMagnitude -= endgameDis(tls_rng);
    }

    // Decidi se aumentare o diminuire la valutazione
    std::uniform_int_distribution<> decisionDis(0, 1);
    bool                            increaseEvaluation = decisionDis(tls_rng) == 1;

    // Applica l'errore alla valutazione di base
    if (increaseEvaluation)
    {
        baseEvaluation += errorMagnitude;
    }
    else
    {
        baseEvaluation -= errorMagnitude;
    }

    // Assicurati che la nuova valutazione rientri nei limiti minimi e massimi definiti
    uint8_t wp = WDLModel::get_win_probability(baseEvaluation, pos);
    baseEvaluation =
      std::clamp(baseEvaluation, min_max_threshold[wp].min_value, min_max_threshold[wp].max_value);

    return baseEvaluation;
}
bool isComplex(const Position& pos) {
    size_t legalMoveCount     = MoveList<LEGAL>(pos).size();
    bool   highMaterial       = pos.non_pawn_material(WHITE) + pos.non_pawn_material(BLACK) > 2400;
    bool   kingDanger         = pos.king_danger(WHITE) || pos.king_danger(BLACK);
    bool   pawnsNearPromotion = ShashinManager::isPawnNearPromotion(pos);
    return ShashinManager::isComplexPosition(legalMoveCount, highMaterial, kingDanger,
                                             pawnsNearPromotion);
}

// Function to compute the adjusted complexity factor based on Elo and game phase
double computeAdjustedComplexityFactor(int uciElo, const Position& pos) {
    // Complexity factor based on position
    double complexityFactor = isComplex(pos) ? 1.0 : 0.5;

    // Factor based on Elo (lower Elo => higher weight for complexity)
    double eloFactor = std::clamp(1.0 - (uciElo - MIN_ELO) / 1870.0, 0.3, 1.0);

    // Factor for opening phase (less complexity weight in early moves)
    double openingFactor = std::clamp(1.0 - (pos.game_ply() / 40.0), 0.3, 1.0);

    // Adjust complexity factor using Elo and opening phase
    return complexityFactor * openingFactor * eloFactor;
}

// Function to decide whether to apply perturbation based on Elo, material, and position complexity
bool should_apply_perturbation(int uciElo, const Position& pos) {
    // Material factor calculation
    const int material = pos.count<PAWN>() + 3 * pos.count<KNIGHT>() + 3 * pos.count<BISHOP>()
                       + 5 * pos.count<ROOK>() + 9 * pos.count<QUEEN>();
    const int materialClamp = std::clamp(material, 17, 78);

    // Base, min thresholds, and ply modifier based on Elo range
    int baseThreshold = 0, minThreshold = 0;

    struct EloRange {
        int MIN_ELO, MAX_ELO;
        int baseThreshold, minThreshold;
    };

    EloRange eloRanges[] = {
      {MIN_ELO, BEGINNER_MAX_ELO, 70, 30},                   // Beginner
      {BEGINNER_MAX_ELO + 1, INTERMEDIATE_MAX_ELO, 30, 15},  // Intermediate
      {INTERMEDIATE_MAX_ELO + 1, ADVANCED_MAX_ELO, 15, 7},   // Advanced
      {ADVANCED_MAX_ELO + 1, MAX_ELO, 7, 3}                  // Expert
    };

    // Determinare i valori di baseThreshold e minThreshold basati sul Elo
    for (const auto& range : eloRanges)
    {
        if (uciElo >= range.MIN_ELO && uciElo <= range.MAX_ELO)
        {
            double t =
              static_cast<double>(uciElo - range.MIN_ELO) / (range.MAX_ELO - range.MIN_ELO);
            baseThreshold = range.baseThreshold * (1 - t) + range.minThreshold * t;
            minThreshold  = range.minThreshold;
            break;
        }
    }

    // Adjusted complexity factor
    double adjustedComplexityFactor = computeAdjustedComplexityFactor(uciElo, pos);

    // Material factor scaled to the range [0.3, 1.0]
    double materialFactor = 1.0 - (materialClamp - 17) / 61.0;  // Maps [17, 78] to [1.0, 0.3]

    // Combine factors to compute the final threshold
    int threshold = std::clamp(
      static_cast<int>(baseThreshold - adjustedComplexityFactor * 10  // Weight for complexity
                       - materialFactor * 5),                         // Weight for material
      minThreshold, baseThreshold);

    // Generate a random number and compare with the threshold
    std::uniform_int_distribution<> dis(0, 100);
    int                             randomFactor = dis(tls_rng);

    return randomFactor < threshold;
}

// Funzione principale
Value get_perturbated_value(const Position& pos, Value baseEvaluation) {
    // Verifica se applicare la perturbazione
    if (should_apply_perturbation(uciElo, pos))
    {
        baseEvaluation = get_handicap_value(baseEvaluation, uciElo, pos);
    }

    return baseEvaluation;
}

// init the true handicap mode
void initHandicapMode(const OptionsMap& options) {
    //true handicap mode begin
    limitStrength = options["UCI_LimitStrength"] || options["LimitStrength_CB"];
    uciElo        = limitStrength ? std::min((int) (options["UCI_Elo"]), (int) (options["ELO_CB"]))
                                  : (int) (MAX_ELO);
    pawnsToEvaluate       = limitStrength ? (uciElo > BEGINNER_MAX_ELO) : 1;
    winnableToEvaluate    = limitStrength ? (uciElo > INTERMEDIATE_MAX_ELO) : 1;
    imbalancesToEvaluate  = limitStrength ? (uciElo > ADVANCED_MAX_ELO) : 1;
    simulateHumanBlunders = limitStrength ? (bool) options["Simulate human blunders"] : false;
    handicappedDepth      = options["Handicapped Depth"];
    initHandicapMinMaxValueThresholds();
    //true handicap mode end
}
// load() reads avatar values
void loadAvatar(const std::string& fname) {

    if (fname.empty())
        return;

    std::ifstream file(fname);
    if (!file)
    {
        std::cerr << "Unable to open avatar file: " << Util::map_path(fname) << std::endl;
        exit(EXIT_FAILURE);
    }

    using WeightsMap = std::map<std::string, int, CaseInsensitiveLess>;
    WeightsMap weightsProperties;

    //Read weights from Avatar file into a map
    std::string line;
    while (std::getline(file, line))
    {

        size_t delimiterPos;
        if (line.empty() || line[0] == '#' || (delimiterPos = line.find('=')) == std::string::npos)
        {
            continue;  // Ignora righe vuote o commenti
        }

        std::string wName  = line.substr(0, delimiterPos);
        std::string wValue = line.substr(delimiterPos + 1);

        try
        {
            int value = std::stoi(wValue);
            if (value < 0 || value > 100)
                throw;

            weightsProperties[wName] = value;
        } catch (...)
        {
            std::cerr << "Avatar option '" << wName << "' with a non weight value: " << wValue
                      << std::endl;
            exit(EXIT_FAILURE);
        }
    }

    file.close();
    if (!fname.empty())
    {
        sync_cout << "info string Avatar file " << fname << " loaded successfully" << sync_endl;
    }
    //Assign to Weights array
    WeightsMap::const_iterator it;
    for (int i = 0; i < AVATAR_NB; ++i)
    {
        if ((it = weightsProperties.find(Weights[i].mgName)) != weightsProperties.end())
            Weights[i].mg = it->second;

        if ((it = weightsProperties.find(Weights[i].egName)) != weightsProperties.end())
            Weights[i].eg = it->second;
    }
}
}  // namespace Eval
}  // namespace Alexander