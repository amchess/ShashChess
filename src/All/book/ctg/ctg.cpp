#include <iostream>
#include <random>
#include <vector>
#include <sstream>
#include <iomanip>
#include "../../movegen.h"
#include "../../uci.h"
#include "ctg.h"

using namespace std;
using namespace Stockfish;

namespace
{
    uint32_t hashCodes[] =
    {
		0x3100D2BF, 0x3118E3DE, 0x34AB1372, 0x2807A847, 0x1633F566, 0x2143B359, 0x26D56488, 0x3B9E6F59,
		0x37755656, 0x3089CA7B, 0x18E92D85, 0x0CD0E9D8,	0x1A9E3B54, 0x3EAA902F, 0x0D9BFAAE, 0x2F32B45B,
		0x31ED6102, 0x3D3C8398, 0x146660E3, 0x0F8D4B76,	0x02C77A5F, 0x146C8799, 0x1C47F51F, 0x249F8F36,
		0x24772043, 0x1FBC1E4D, 0x1E86B3FA, 0x37DF36A6,	0x16ED30E4, 0x02C3148E, 0x216E5929, 0x0636B34E,
		0x317F9F56, 0x15F09D70, 0x131026FB, 0x38C784B1,	0x29AC3305, 0x2B485DC5, 0x3C049DDC, 0x35A9FBCD,
		0x31D5373B, 0x2B246799, 0x0A2923D3, 0x08A96E9D,	0x30031A9F, 0x08F525B5, 0x33611C06, 0x2409DB98,
		0x0CA4FEB2, 0x1000B71E, 0x30566E32, 0x39447D31,	0x194E3752, 0x08233A95, 0x0F38FE36, 0x29C7CD57,
		0x0F7B3A39, 0x328E8A16, 0x1E7D1388, 0x0FBA78F5,	0x274C7E7C, 0x1E8BE65C, 0x2FA0B0BB, 0x1EB6C371
    };

    struct MoveEnc
    {
        unsigned char encoding;
        char piece;
        int32_t num;
        int32_t forward, right;
    } moveTable[] =
    {
        (unsigned char)0x00, 'P', 5,  1,  1,
        (unsigned char)0x01, 'N', 2, -1, -2,
        (unsigned char)0x03, 'Q', 2,  0,  2,
        (unsigned char)0x04, 'P', 2,  1,  0,
        (unsigned char)0x05, 'Q', 1,  1,  0,
        (unsigned char)0x06, 'P', 4,  1, -1,
        (unsigned char)0x08, 'Q', 2,  0,  4,
        (unsigned char)0x09, 'B', 2,  6,  6,
        (unsigned char)0x0A, 'K', 1, -1,  0,
        (unsigned char)0x0C, 'P', 1,  1, -1,
        (unsigned char)0x0D, 'B', 1,  3,  3,
        (unsigned char)0x0E, 'R', 2,  0,  3,
        (unsigned char)0x0F, 'N', 1, -1, -2,
        (unsigned char)0x12, 'B', 1,  7,  7,
        (unsigned char)0x13, 'K', 1,  1,  0,
        (unsigned char)0x14, 'P', 8,  1,  1,
        (unsigned char)0x15, 'B', 1,  5,  5,
        (unsigned char)0x18, 'P', 7,  1,  0,
        (unsigned char)0x1A, 'Q', 2,  6,  0,
        (unsigned char)0x1B, 'B', 1,  1, -1,
        (unsigned char)0x1D, 'B', 2,  7,  7,
        (unsigned char)0x21, 'R', 2,  0,  7,
        (unsigned char)0x22, 'B', 2,  2, -2,
        (unsigned char)0x23, 'Q', 2,  6,  6,
        (unsigned char)0x24, 'P', 8,  1, -1,
        (unsigned char)0x26, 'B', 1,  7, -7,
        (unsigned char)0x27, 'P', 3,  1, -1,
        (unsigned char)0x28, 'Q', 1,  5,  5,
        (unsigned char)0x29, 'Q', 1,  0,  6,
        (unsigned char)0x2A, 'N', 2, -2,  1,
        (unsigned char)0x2D, 'P', 6,  1,  1,
        (unsigned char)0x2E, 'B', 1,  1,  1,
        (unsigned char)0x2F, 'Q', 1,  0,  1,
        (unsigned char)0x30, 'N', 2, -2, -1,
        (unsigned char)0x31, 'Q', 1,  0,  3,
        (unsigned char)0x32, 'B', 2,  5,  5,
        (unsigned char)0x34, 'N', 1,  2,  1,
        (unsigned char)0x36, 'N', 1,  1,  2,
        (unsigned char)0x37, 'Q', 1,  4,  0,
        (unsigned char)0x38, 'Q', 2,  4, -4,
        (unsigned char)0x39, 'Q', 1,  0,  5,
        (unsigned char)0x3A, 'B', 1,  6,  6,
        (unsigned char)0x3B, 'Q', 2,  5, -5,
        (unsigned char)0x3C, 'B', 1,  5, -5,
        (unsigned char)0x41, 'Q', 2,  5,  5,
        (unsigned char)0x42, 'Q', 1,  7, -7,
        (unsigned char)0x44, 'K', 1, -1,  1,
        (unsigned char)0x45, 'Q', 1,  3,  3,
        (unsigned char)0x4A, 'P', 8,  2,  0,
        (unsigned char)0x4B, 'Q', 1,  5, -5,
        (unsigned char)0x4C, 'N', 2,  2,  1,
        (unsigned char)0x4D, 'Q', 2,  1,  0,
        (unsigned char)0x50, 'R', 1,  6,  0,
        (unsigned char)0x52, 'R', 1,  0,  6,
        (unsigned char)0x54, 'B', 2,  1, -1,
        (unsigned char)0x55, 'P', 3,  1,  0,
        (unsigned char)0x5C, 'P', 7,  1,  1,
        (unsigned char)0x5F, 'P', 5,  2,  0,
        (unsigned char)0x61, 'Q', 1,  6,  6,
        (unsigned char)0x62, 'P', 2,  2,  0,
        (unsigned char)0x63, 'Q', 2,  7, -7,
        (unsigned char)0x66, 'B', 1,  3, -3,
        (unsigned char)0x67, 'K', 1,  1,  1,
        (unsigned char)0x69, 'R', 2,  7,  0,
        (unsigned char)0x6A, 'B', 1,  4,  4,
        (unsigned char)0x6B, 'K', 1,  0,  2,   /* short castling */
        (unsigned char)0x6E, 'R', 1,  0,  5,
        (unsigned char)0x6F, 'Q', 2,  7,  7,
        (unsigned char)0x72, 'B', 2,  7, -7,
        (unsigned char)0x74, 'Q', 1,  0,  2,
        (unsigned char)0x79, 'B', 2,  6, -6,
        (unsigned char)0x7A, 'R', 1,  3,  0,
        (unsigned char)0x7B, 'R', 2,  6,  0,
        (unsigned char)0x7C, 'P', 3,  1,  1,
        (unsigned char)0x7D, 'R', 2,  1,  0,
        (unsigned char)0x7E, 'Q', 1,  3, -3,
        (unsigned char)0x7F, 'R', 1,  0,  1,
        (unsigned char)0x80, 'Q', 1,  6, -6,
        (unsigned char)0x81, 'R', 1,  1,  0,
        (unsigned char)0x82, 'P', 6,  1, -1,
        (unsigned char)0x85, 'N', 1,  2, -1,
        (unsigned char)0x86, 'R', 1,  0,  7,
        (unsigned char)0x87, 'R', 1,  5,  0,
        (unsigned char)0x8A, 'N', 1, -2,  1,
        (unsigned char)0x8B, 'P', 1,  1,  1,
        (unsigned char)0x8C, 'K', 1, -1, -1,
        (unsigned char)0x8E, 'Q', 2,  2, -2,
        (unsigned char)0x8F, 'Q', 1,  0,  7,
        (unsigned char)0x92, 'Q', 2,  1,  1,
        (unsigned char)0x94, 'Q', 1,  3,  0,
        (unsigned char)0x96, 'P', 2,  1,  1,
        (unsigned char)0x97, 'K', 1,  0, -1,
        (unsigned char)0x98, 'R', 1,  0,  3,
        (unsigned char)0x99, 'R', 1,  4,  0,
        (unsigned char)0x9A, 'Q', 1,  6,  0,
        (unsigned char)0x9B, 'P', 3,  2,  0,
        (unsigned char)0x9D, 'Q', 1,  2,  0,
        (unsigned char)0x9F, 'B', 2,  4, -4,
        (unsigned char)0xA0, 'Q', 2,  3,  0,
        (unsigned char)0xA2, 'Q', 1,  2,  2,
        (unsigned char)0xA3, 'P', 8,  1,  0,
        (unsigned char)0xA5, 'R', 2,  5,  0,
        (unsigned char)0xA9, 'R', 2,  0,  2,
        (unsigned char)0xAB, 'Q', 2,  6, -6,
        (unsigned char)0xAD, 'R', 2,  0,  4,
        (unsigned char)0xAE, 'Q', 2,  3,  3,
        (unsigned char)0xB0, 'Q', 2,  4,  0,
        (unsigned char)0xB1, 'P', 6,  2,  0,
        (unsigned char)0xB2, 'B', 1,  6, -6,
        (unsigned char)0xB5, 'R', 2,  0,  5,
        (unsigned char)0xB7, 'Q', 1,  5,  0,
        (unsigned char)0xB9, 'B', 2,  3,  3,
        (unsigned char)0xBB, 'P', 5,  1,  0,
        (unsigned char)0xBC, 'Q', 2,  0,  5,
        (unsigned char)0xBD, 'Q', 2,  2,  0,
        (unsigned char)0xBE, 'K', 1,  0,  1,
        (unsigned char)0xC1, 'B', 1,  2,  2,
        (unsigned char)0xC2, 'B', 2,  2,  2,
        (unsigned char)0xC3, 'B', 1,  2, -2,
        (unsigned char)0xC4, 'R', 2,  0,  1,
        (unsigned char)0xC5, 'R', 2,  4,  0,
        (unsigned char)0xC6, 'Q', 2,  5,  0,
        (unsigned char)0xC7, 'P', 7,  1, -1,
        (unsigned char)0xC8, 'P', 7,  2,  0,
        (unsigned char)0xC9, 'Q', 2,  7,  0,
        (unsigned char)0xCA, 'B', 2,  3, -3,
        (unsigned char)0xCB, 'P', 6,  1,  0,
        (unsigned char)0xCC, 'B', 2,  5, -5,
        (unsigned char)0xCD, 'R', 1,  0,  2,
        (unsigned char)0xCF, 'P', 4,  1,  0,
        (unsigned char)0xD1, 'P', 2,  1, -1,
        (unsigned char)0xD2, 'N', 2,  1,  2,
        (unsigned char)0xD3, 'N', 2,  1, -2,
        (unsigned char)0xD7, 'Q', 1,  1, -1,
        (unsigned char)0xD8, 'R', 2,  0,  6,
        (unsigned char)0xD9, 'Q', 1,  2, -2,
        (unsigned char)0xDA, 'N', 1, -2, -1,
        (unsigned char)0xDB, 'P', 1,  2,  0,
        (unsigned char)0xDE, 'P', 5,  1, -1,
        (unsigned char)0xDF, 'K', 1,  1, -1,
        (unsigned char)0xE0, 'N', 2, -1,  2,
        (unsigned char)0xE1, 'R', 1,  7,  0,
        (unsigned char)0xE3, 'R', 2,  3,  0,
        (unsigned char)0xE5, 'Q', 1,  0,  4,
        (unsigned char)0xE6, 'P', 4,  2,  0,
        (unsigned char)0xE7, 'Q', 1,  4,  4,
        (unsigned char)0xE8, 'R', 1,  2,  0,
        (unsigned char)0xE9, 'N', 1, -1,  2,
        (unsigned char)0xEB, 'P', 4,  1,  1,
        (unsigned char)0xEC, 'P', 1,  1,  0,
        (unsigned char)0xED, 'Q', 1,  7,  7,
        (unsigned char)0xEE, 'Q', 2,  1, -1,
        (unsigned char)0xEF, 'R', 1,  0,  4,
        (unsigned char)0xF0, 'Q', 2,  0,  7,
        (unsigned char)0xF1, 'Q', 1,  1,  1,
        (unsigned char)0xF3, 'N', 2,  2, -1,
        (unsigned char)0xF4, 'R', 2,  2,  0,
        (unsigned char)0xF5, 'B', 2,  1,  1,
        (unsigned char)0xF6, 'K', 1,  0, -2,   /* long castling */
        (unsigned char)0xF7, 'N', 1,  1, -2,
        (unsigned char)0xF8, 'Q', 2,  0,  1,
        (unsigned char)0xF9, 'Q', 2,  6,  0,
        (unsigned char)0xFA, 'Q', 2,  0,  3,
        (unsigned char)0xFB, 'Q', 2,  2,  2,
        (unsigned char)0xFD, 'Q', 1,  7,  0,
        (unsigned char)0xFE, 'Q', 2,  3, -3
    };

	constexpr size_t MoveEncSize = sizeof(moveTable) / sizeof(moveTable[0]);
}

namespace
{
	static default_random_engine randomEngine = default_random_engine(now());

	enum class CtgMoveAnnotation
	{
		None = 0x00,
		GoodMove = 0x01, //!
		BadMove = 0x02, //?
		ExcellentMove = 0x03, //!!
		LosingMove = 0x04, //??
		InterestingMove = 0x05, //!?
		DubiousMove = 0x06, //?!
		OnlyMove = 0x08,
		Zugzwang = 0x16,
		Unknown = 0xFF,
	};

	enum class CtgMoveRecommendation
	{
		NoPreference = 0x00,
		RedMove = 0x40,
		GreenMove = 0x80,
		Unknown = 0xFF,
	};

	enum class CtgMoveCommentary
	{
		None = 0x00,
		Equal = 0x0B, //=
		Unclear = 0x0D,
		EqualPlus = 0x0E, //=+
		PlusEqual = 0x0F, //+=
		MinusSlashPlus = 0x10, //-/+
		PlusSlashMinus = 0x11, //+/-
		PlusMinus = 0x13, //+-
		DevelopmentAdvantage = 0x20,
		Initiative = 0x24,
		WithAttack = 0x28,
		Compensation = 0x2C,
		Counterplay = 0x84,
		Zeitnot = 0x8A,
		Novelty = 0x92,
		Unknown = 0xFF,
	};

	struct CtgMoveStats
	{
		int32_t win;
		int32_t loss;
		int32_t draw;

		int32_t ratingDiv;
		int32_t ratingSum;

		CtgMoveStats()
		{
			win = 0;
			loss = 0;
			draw = 0;

			ratingDiv = 0;
			ratingSum = 0;
		}
	};

	struct CtgMove : CtgMoveStats
	{
	private:
		Move      pseudoMove;
		Move      sfMove;

	public:
		CtgMoveAnnotation     annotation;
		CtgMoveRecommendation recommendation;
		CtgMoveCommentary     commentary;

		int64_t				  moveWeight;

		CtgMove() : CtgMoveStats()
		{
			pseudoMove     = MOVE_NONE;
			sfMove         = MOVE_NONE;

			annotation     = CtgMoveAnnotation::Unknown;
			recommendation = CtgMoveRecommendation::Unknown;
			commentary     = CtgMoveCommentary::Unknown;

			moveWeight     = numeric_limits<int64_t>::min();
		}

		void set_from_to(const Position& pos, Square from, Square to)
		{
			PieceType promotionPiece = NO_PIECE_TYPE;

			//Special handling of castling moves : SF encodes castling as KxR, while CTG encodes it as King moving in the direction of the rook by two steps
			//Special handling of promotion moves: This CTG implementation does not support underpromotion
			if (from == SQ_E1 && to == SQ_G1 && pos.piece_on(from) == W_KING && pos.piece_on(SQ_H1) == W_ROOK && pos.can_castle(WHITE_OO))
				to = SQ_H1;
			else if (from == SQ_E8 && to == SQ_G8 && pos.piece_on(from) == B_KING && pos.piece_on(SQ_H8) == B_ROOK && pos.can_castle(BLACK_OO))
				to = SQ_H8;
			else if (from == SQ_E1 && to == SQ_C1 && pos.piece_on(from) == W_KING && pos.piece_on(SQ_A1) == W_ROOK && pos.can_castle(WHITE_OOO))
				to = SQ_A1;
			else if (from == SQ_E8 && to == SQ_C8 && pos.piece_on(from) == B_KING && pos.piece_on(SQ_A8) == B_ROOK && pos.can_castle(BLACK_OOO))
				to = SQ_A8;
			else if (((rank_of(from) == RANK_7 && rank_of(to) == RANK_8) || (rank_of(from) == RANK_2 && rank_of(to) == RANK_1)) && type_of(pos.piece_on(from)) == PAWN)
				promotionPiece = QUEEN;

			pseudoMove = promotionPiece == NO_PIECE_TYPE ? make_move(from, to) : make<PROMOTION>(from, to, promotionPiece);
		}

		Move pseudo_move() const
		{
			assert(pseudoMove != MOVE_NONE);
			return pseudoMove;
		}

		Move set_sf_move(Move m)
		{
			return sfMove = m;
		}

		Move sf_move() const
		{
			assert(sfMove != MOVE_NONE);
			return sfMove;
		}

		int64_t weight() const
		{
			assert(moveWeight != numeric_limits<int64_t>::min());
			return moveWeight;
		}

		bool green() const
		{
			return    ((int)recommendation & (int)CtgMoveRecommendation::GreenMove)
				   && annotation     != CtgMoveAnnotation::BadMove
			       && annotation     != CtgMoveAnnotation::LosingMove
				   && annotation     != CtgMoveAnnotation::InterestingMove
				   && annotation     != CtgMoveAnnotation::DubiousMove;
		}

		bool red() const
		{
			return ((int)recommendation & (int)CtgMoveRecommendation::RedMove);
		}
	};

	struct CtgMoveList : public vector<CtgMove>
	{
		CtgMoveStats    positionStats;

		void calculate_weights()
		{
			if (size() == 0)
				return;

			auto calculate_pseudo_weight = [](CtgMove& m, int win, int loss, int draw) -> int64_t
			{
				static constexpr int64_t MAX_WEIGHT = numeric_limits<int16_t>::max();
				static constexpr int64_t MIN_WEIGHT = numeric_limits<int16_t>::min();

				int64_t winFactor = 2;
				int64_t lossFactor = 2;
				constexpr int64_t drawFactor = 1;

				//Recommendation
				winFactor += m.green() ? 10 : 0;
				lossFactor += m.red() ? 10 : 0;

				//Annotation
				switch (m.annotation)
				{
				case CtgMoveAnnotation::GoodMove:
					winFactor += m.green() ? 5 : 0;
					break;

				case CtgMoveAnnotation::BadMove:
					lossFactor += 5;
					break;

				case CtgMoveAnnotation::ExcellentMove:
					winFactor += m.green() ? 10 : 0;
					break;

				case CtgMoveAnnotation::LosingMove:
					lossFactor += 10;
					break;

				case CtgMoveAnnotation::InterestingMove:
					winFactor += 2;
					break;

				case CtgMoveAnnotation::DubiousMove:
					lossFactor += 2;
					break;

				case CtgMoveAnnotation::Zugzwang:
					winFactor  += 1;
					lossFactor += 1;
					break;

				case CtgMoveAnnotation::OnlyMove:
					winFactor += m.green() ? MAX_WEIGHT : 0;
					break;

				default: //Just to avoid GCC warning: enumeration value 'XXX' not handled in switch
					break;
				}

				if (winFactor == MAX_WEIGHT)
					return MAX_WEIGHT;

				if (lossFactor == MAX_WEIGHT)
					return MIN_WEIGHT;

				return ((win + 100) * winFactor - (loss + 100) * lossFactor + (draw + 100) * drawFactor);
			};

			//Calculate average number of games
			int64_t avgGames = 0;
			for (const CtgMove& m : *this)
				avgGames += m.win + m.loss + m.draw;

			avgGames /= size();
			if (avgGames == 0)
				avgGames = 300;

			//Calculate weight assuming all moves have played the
			// calculated average number of games, by adding (or removing)
			// an equal number of won, lost, and drawn games to each move
			//Also calculate minimum and maximum for normalization later
			int64_t maxWeight = numeric_limits<int64_t>::min();
			int64_t minWeight = numeric_limits<int64_t>::max();
			for (CtgMove& m : *this)
			{
				int64_t games = m.win + m.loss + m.draw;
				int64_t diff = (avgGames - games) / 3;

				int64_t win  = max<int64_t>(m.win  + diff, 0);
				int64_t loss = max<int64_t>(m.loss + diff, 0);
				int64_t draw = max<int64_t>(m.draw + diff, 0);

				assert(win + draw + loss >= 0);
				if (win + loss + draw == 0)
					m.moveWeight = 0;
				else
					m.moveWeight = calculate_pseudo_weight(m, win, loss, draw);

				if (m.moveWeight < minWeight)
					minWeight = m.moveWeight;

				if (m.moveWeight > maxWeight)
					maxWeight = m.moveWeight;
			}

			//Normalize to [-100, +100]
			for (CtgMove& m : *this)
			{
				if (maxWeight == minWeight)
				{
					m.moveWeight = 0;
					continue;
				}

				m.moveWeight = (m.moveWeight - minWeight) * 200 / (maxWeight - minWeight) - 100;
			}

			//Sort
			stable_sort(begin(), end(), [](const CtgMove& mv1, const CtgMove& mv2) { return mv1.weight() > mv2.weight(); });
		}
	};

	struct CtgPositionData
	{
		Square           epSquare;
		bool             invert;
		bool             flip;
		char             board[64];

		unsigned char    encodedPosition[32];
		int32_t          encodedPosLen;
		int32_t          encodedBitsLeft;

		unsigned char    positionPage[256];

	public:
		CtgPositionData()
		{
			epSquare = SQ_NONE;
			invert = false;
			flip = false;

			memset(board, 0, sizeof(board));
			memset(encodedPosition, 0, sizeof(encodedPosition));

			encodedPosLen = 0;
			encodedBitsLeft = 0;

			memset(positionPage, 0, sizeof(positionPage));
		}

		CtgPositionData(const CtgPositionData&) = delete;
		CtgPositionData& operator =(const CtgPositionData&) = delete;
	};
}

namespace Stockfish::Book::CTG
{
	bool CtgBook::decode(const Position& pos, CtgPositionData& positionData) const
	{
		positionData.epSquare = pos.ep_square();
		positionData.invert = pos.side_to_move() == BLACK;
		positionData.flip = needs_flipping(pos);

		//Decode
		decode_board(pos, positionData);

		//Invert?
		if (positionData.invert)
			invert_board(positionData);

		//Flip?
		if (positionData.flip)
			flip_board(pos, positionData);

		//Encode
		encode_position(pos, positionData);

		//Lookup position page and data
		if (!lookup_position(positionData))
			return false;

		return true;
	}

	void CtgBook::decode_board(const Position& pos, CtgPositionData& positionData) const
	{
		static constexpr string_view PieceToChar(" PNBRQK  pnbrqk");

		//Clear the internal board representation
		memset(positionData.board, 0, sizeof(positionData.board));

		//Loop Ranks and Files from Rank 8 to Rank 1, and from File A to File H
		size_t index = 0;
		for (Rank r = RANK_8; r >= RANK_1; --r)
		{
			for (File f = FILE_A; f <= FILE_H; ++f)
			{
				Piece p = pos.piece_on(make_square(f, r));
				positionData.board[index++] = PieceToChar[p];
			}
		}
	}

	void CtgBook::invert_board(CtgPositionData& positionData) const
	{
		//Flip
		for (size_t y = 0; y < 4; ++y)
		{
			for (size_t x = 0; x < 8; ++x)
			{
				char tmp = positionData.board[y * 8 + (x)];
				positionData.board[y * 8 + (x)] = positionData.board[(7 - y) * 8 + (x)];
				positionData.board[(7 - y) * 8 + (x)] = tmp;
			}
		}

		//Invert colors
		for (size_t y = 0; y < 8; ++y)
		{
			for (size_t x = 0; x < 8; ++x)
			{
				char& p = positionData.board[y * 8 + x];

				if (p == ' ')
					continue;

				if (p == toupper(p))
					p = tolower(p);
				else
					p = toupper(p);
			}
		}
	}

	//Board should be flipped if:
	//1) No side can castle, and
	//2) White King is on the left side of the board (ranks RANK_1, RANK_2, RANK_3, RANK_4)
	bool CtgBook::needs_flipping(const Position& pos) const
	{
		if (pos.can_castle(ANY_CASTLING))
			return false;

		Square ksq = pos.square<KING>(WHITE);

		return file_of(ksq) <= FILE_D;
	}

	void CtgBook::flip_board(const Position& pos, CtgPositionData& positionData) const
	{
		//Flip horizontally
		for (size_t y = 0; y < 8; ++y)
		{
			for (size_t x = 0; x < 4; ++x)
			{
				char tmp = positionData.board[y * 8 + x];
				positionData.board[y * 8 + (x)] = positionData.board[y * 8 + (7 - x)];
				positionData.board[y * 8 + (7 - x)] = tmp;
			}
		}

		//Flip the en passant square
		Square epSquare = pos.ep_square();
		if (epSquare != SQ_NONE)
			epSquare = flip_rank(epSquare);
	}

	void CtgBook::encode_position(const Position& pos, CtgPositionData& positionData) const
	{
		auto put_bit = [&](int32_t b)
		{
			positionData.encodedPosition[positionData.encodedPosLen] <<= 1;
			if (b)
				positionData.encodedPosition[positionData.encodedPosLen] |= 1;

			if (--positionData.encodedBitsLeft == 0)
			{
				++positionData.encodedPosLen;
				positionData.encodedBitsLeft = 8;
			}
		};

		//Encoded position length is at index = 0
		positionData.encodedPosLen = 1;
		positionData.encodedBitsLeft = 8;

		//Pit-packing of encoded position
		for (size_t x = 0; x < 8; ++x)
		{
			for (size_t y = 0; y < 8; ++y)
			{
				switch (positionData.board[(7 - y) * 8 + x])
				{
				case ' ':
					put_bit(0);
					break;
				case 'p':
					put_bit(1);
					put_bit(1);
					put_bit(1);
					break;
				case 'P':
					put_bit(1);
					put_bit(1);
					put_bit(0);
					break;
				case 'r':
					put_bit(1);
					put_bit(0);
					put_bit(1);
					put_bit(1);
					put_bit(1);
					break;
				case 'R':
					put_bit(1);
					put_bit(0);
					put_bit(1);
					put_bit(1);
					put_bit(0);
					break;
				case 'b':
					put_bit(1);
					put_bit(0);
					put_bit(1);
					put_bit(0);
					put_bit(1);
					break;
				case 'B':
					put_bit(1);
					put_bit(0);
					put_bit(1);
					put_bit(0);
					put_bit(0);
					break;
				case 'n':
					put_bit(1);
					put_bit(0);
					put_bit(0);
					put_bit(1);
					put_bit(1);
					break;
				case 'N':
					put_bit(1);
					put_bit(0);
					put_bit(0);
					put_bit(1);
					put_bit(0);
					break;
				case 'q':
					put_bit(1);
					put_bit(0);
					put_bit(0);
					put_bit(0);
					put_bit(1);
					put_bit(1);
					break;
				case 'Q':
					put_bit(1);
					put_bit(0);
					put_bit(0);
					put_bit(0);
					put_bit(1);
					put_bit(0);
					break;
				case 'k':
					put_bit(1);
					put_bit(0);
					put_bit(0);
					put_bit(0);
					put_bit(0);
					put_bit(1);
					break;
				case 'K':
					put_bit(1);
					put_bit(0);
					put_bit(0);
					put_bit(0);
					put_bit(0);
					put_bit(0);
					break;
				}
			}
		}

		bool epAny = false;
		[[maybe_unused]] File epFile = FILE_NB;
		if (positionData.epSquare != SQ_NONE)
		{
			epFile = file_of(positionData.epSquare);

			if ((epFile > FILE_A && positionData.board[3 * 8 + epFile - 1] == 'P') || (epFile < FILE_G && positionData.board[3 * 8 + epFile + 1] == 'P'))
			{
				epAny = true;
			}
		}

		//Find the right number of bits
		int32_t right = epAny ? 3 : 8;

		//Castling needs four more
		if (pos.can_castle(ANY_CASTLING))
		{
			right = right + 4;
			if (right > 8)
				right %= 8;
		}

		int32_t nb = 0;
		if (positionData.encodedBitsLeft > right)
			nb = positionData.encodedBitsLeft - right;
		else if (positionData.encodedBitsLeft < right)
			nb = positionData.encodedBitsLeft + 8 - right;

		if (positionData.encodedBitsLeft == 8 && !pos.can_castle(ANY_CASTLING) && !epAny)
			nb = 8;

		for (int32_t i = 0; i < nb; ++i)
			put_bit(0);

		// En passant
		if (epAny)
		{
			put_bit(epFile & 0x04);
			put_bit(epFile & 0x02);
			put_bit(epFile & 0x01);
		}

		//Castling rights
		if (pos.can_castle(ANY_CASTLING))
		{
			put_bit(pos.can_castle(positionData.invert ? WHITE_OO : BLACK_OO) ? 1 : 0);
			put_bit(pos.can_castle(positionData.invert ? WHITE_OOO : BLACK_OOO) ? 1 : 0);
			put_bit(pos.can_castle(positionData.invert ? BLACK_OO : WHITE_OO) ? 1 : 0);
			put_bit(pos.can_castle(positionData.invert ? BLACK_OOO : WHITE_OOO) ? 1 : 0);
		}

		//Padding stuff
		while (positionData.encodedBitsLeft != 8)
			put_bit(0);

		//Header byte
		positionData.encodedPosition[0] = positionData.encodedPosLen;

		if (pos.can_castle(ANY_CASTLING))
			positionData.encodedPosition[0] |= 0x40;

		if (epAny)
			positionData.encodedPosition[0] |= 0x20;
	}

	bool CtgBook::read_position_data(CtgPositionData& positionData, uint32_t pageNum) const
	{
		uint32_t pagePos = BookUtil::read_big_endian<uint32_t>(cto.data() + pageNum * 4 + 16, cto.data_size());
		if (pagePos == 0xFFFFFFFF)
			return false;

		if ((pagePos + 2) * 4096 > ctg.data_size())
			return false;

		unsigned char pageData[4096];
		memcpy(pageData, ctg.data() + (pagePos + 1) * 4096, 4096);

		uint16_t pageLength = BookUtil::read_big_endian<uint16_t>((const unsigned char*)pageData + 2, sizeof(pageData));

		if (pageLength > 4096)
		{
			assert(false);
			return false;
		}

		uint32_t posOffset = 4;
		while (posOffset < pageLength)
		{
			if (pageData[posOffset] != positionData.encodedPosition[0] || memcmp(pageData + posOffset, positionData.encodedPosition, positionData.encodedPosLen) != 0)
			{
				//No match -> skip to next page
				posOffset += pageData[posOffset] & 0x1F;
				posOffset += pageData[posOffset];
				posOffset += 33;

				continue;
			}

			posOffset += pageData[posOffset] & 0x1F;

			if (posOffset + pageData[posOffset] + 33 > pageLength)
				return false;

			memcpy(positionData.positionPage, pageData + posOffset, pageData[posOffset] + 33);

			return true;
		}

		return false;
	}

	uint32_t CtgBook::gen_position_hash(CtgPositionData& positionData) const
	{
		int32_t hash = 0;
		int16_t tmp = 0;

		for (int32_t i = 0; i < positionData.encodedPosLen; ++i)
		{
			char ch = positionData.encodedPosition[i];

			tmp += ((0x0F - (ch & 0x0F)) << 2) + 1;
			hash += hashCodes[tmp & 0x3F];

			tmp += ((0xF0 - (ch & 0xF0)) >> 2) + 1;
			hash += hashCodes[tmp & 0x3F];
		}

		return hash;
	}

	bool CtgBook::lookup_position(CtgPositionData& positionData) const
	{
		uint32_t hash = gen_position_hash(positionData);

		for (int32_t mask = 0; mask < 0x7FFFFFFF; mask = 2 * mask + 1)
		{
			uint32_t pageNum = (hash & mask) + mask;

			if (pageNum < pageLowerBound)
				continue;

			if (read_position_data(positionData, pageNum))
				return true;

			if (pageNum >= pageUpperBound)
				break;
		}

		return false;
	}

	void CtgBook::get_stats(const CtgPositionData& positionData, CtgMoveStats& stats, bool isMove) const
	{
		const unsigned char* posPageData = positionData.positionPage;
		posPageData += *posPageData;
		posPageData += 3;

		//Win-loss-draw
		stats.win = (posPageData[0] << 16) | (posPageData[1] << 8) | posPageData[2];
		stats.loss = (posPageData[3] << 16) | (posPageData[4] << 8) | posPageData[5];
		stats.draw = (posPageData[6] << 16) | (posPageData[7] << 8) | posPageData[8];

		if (positionData.invert)
		{
			int temp = stats.win;
			stats.win = stats.loss;
			stats.loss = temp;
		}

		//Rating
		posPageData += 9 + 4 + 7;
		stats.ratingDiv = (posPageData[0] << 16) | (posPageData[1] << 8) | posPageData[2];
		stats.ratingSum = (posPageData[3] << 24) | (posPageData[4] << 16) | (posPageData[5] << 8) | posPageData[6];

		if (!isMove)
			return;

		CtgMove& ctgMove = (CtgMove&)stats;

		//Recommendations
		ctgMove.recommendation = (CtgMoveRecommendation)positionData.positionPage[(uint32_t)positionData.positionPage[0] + 3 + 9 + 4 + 7 + 7];

		//Commentary
		ctgMove.commentary = (CtgMoveCommentary)positionData.positionPage[(uint32_t)positionData.positionPage[0] + 3 + 9 + 4 + 7 + 7 + 1];
	}

	Move CtgBook::get_pseudo_move(const CtgPositionData& positionData, int moveNum) const
	{
		unsigned char encodedMove = positionData.positionPage[moveNum * 2 + 1];

		//Search
		size_t index = 0;
		while (index < MoveEncSize)
		{
			if (encodedMove == moveTable[index].encoding)
				break;

			++index;
		}

		//Check
		if (index == MoveEncSize)
			return MOVE_NONE;

		//Find/Read the move
		const MoveEnc& moveEnc = moveTable[index];
		int num = moveEnc.num;
		for (int x = 0; x < 8; ++x)
		{
			for (int y = 0; y < 8; ++y)
			{
				if (positionData.board[(7 - y) * 8 + x] != moveEnc.piece)
					continue;

				if (--num == 0)
				{
					Square from = make_square(File(x), Rank(y));
					Square to = make_square(File((x + 8 + moveEnc.right) % 8), Rank((y + 8 + moveEnc.forward) % 8));

					return make_move(from, to);
				}
			}
		}

		//Should never get here
		assert(false);
		return MOVE_NONE;
	}

	bool CtgBook::get_move(const Position& pos, const CtgPositionData& positionData, int moveNum, CtgMove& ctgMove) const
	{
		Move m = get_pseudo_move(positionData, moveNum);
		if (m == MOVE_NONE)
			return false;

		Square from = from_sq(m);
		Square to   = to_sq(m);

		if (positionData.invert)
		{
			from = flip_rank(from);
			to   = flip_rank(to);
		}

		if (positionData.flip)
		{
			from = flip_file(from);
			to   = flip_file(to);
		}

		//Assign
		ctgMove.set_from_to(pos, from, to);

		//Annotation
		ctgMove.annotation = (CtgMoveAnnotation)positionData.positionPage[moveNum * 2 + 2];

		return true;
	}

	void CtgBook::get_moves(const Position& pos, const CtgPositionData& positionData, CtgMoveList& ctgMoveList) const
	{
		//Get legal moves for cross checking later
		MoveList legalMoves = MoveList<LEGAL>(pos);	

		//Position object to be used to play the moves
		StateInfo si[2];
		Position p;
		p.set(pos.fen(), pos.is_chess960(), &si[0], pos.this_thread());

		//Read position statistics
		get_stats(positionData, ctgMoveList.positionStats, false);

		int32_t movesCount = positionData.positionPage[0] >> 1;
		for (int i = 0; i < movesCount; ++i)
		{
			CtgMove ctgMove;
			if (get_move(pos, positionData, i, ctgMove))
			{
				for (const auto& m : legalMoves)
				{
					if (ctgMove.pseudo_move() == (m.move ^ type_of(m.move)))
					{
						//Assign the move
						ctgMove.set_sf_move(m.move);

						//Play the move
						p.do_move(ctgMove.sf_move(), si[1]);

						//Decode and get move info from the new position
						CtgPositionData pd;
						if (decode(p, pd))
							get_stats(pd, ctgMove, true);

						//Undo move
						p.undo_move(ctgMove.sf_move());

						//Add to list
						ctgMoveList.push_back(ctgMove);
						break;
					}
				}

				assert(ctgMove.sf_move() != MOVE_NONE);
			}
		}

		//Calculate move weights
		ctgMoveList.calculate_weights();
	}

	CtgBook::CtgBook() : cto(), ctg(), pageLowerBound(0), pageUpperBound(0), isOpen(false)
	{
	}

	CtgBook::~CtgBook()
	{
		close();
	}

	string CtgBook::type() const
	{
		return "CTG";
	}

	bool CtgBook::open(const string& f)
	{
		//Close current file
		close();

		//If no file name is given -> nothing to do
		if (Utility::is_empty_filename(f))
			return true;

		//CTG
		string fn = Utility::map_path(f);
		string ctgFile = fn.substr(0, fn.find_last_of('.')) + ".ctg";
		if (!ctg.map(ctgFile.c_str(), true))
		{
			close();

			sync_cout << "info string Could not open CTG file: " << ctgFile << sync_endl;
			return false;
		}

		//CTO
		string ctoFile = ctgFile.substr(0, ctgFile.find_last_of('.')) + ".cto";
		if (!cto.map(ctoFile.c_str(), true))
		{
			close();

			sync_cout << "info string Could not open CTO file: " << ctoFile << sync_endl;
			return false;
		}

		//CTB
		string ctbFile = ctgFile.substr(0, ctgFile.find_last_of('.')) + ".ctb";
		Utility::FileMapping ctb;
		if (!ctb.map(ctbFile.c_str(), true))
		{
			close();

			sync_cout << "info string Could not open CTB file: " << ctbFile << sync_endl;
			return false;
		}

		//Read page bounds from CTB
		size_t offset = 4;
		pageLowerBound = BookUtil::read_big_endian<uint32_t>(ctb.data(), offset, ctb.data_size());
		pageUpperBound = BookUtil::read_big_endian<uint32_t>(ctb.data(), offset, ctb.data_size());

		//We no longer need the CTB file
		ctb.unmap();

		isOpen = true;

		sync_cout << "info string CTG Book [" << ctgFile << "] opened successfully" << sync_endl;
		return true;
	}

	void CtgBook::close()
	{
		ctg.unmap();
		cto.unmap();

		pageLowerBound = 0;
		pageUpperBound = 0;

		isOpen = false;
	}

	bool CtgBook::is_open() const
	{
		return isOpen;
	}

	Move CtgBook::probe(const Position& pos, size_t width, bool onlyGreen) const
	{
		if (!is_open())
			return MOVE_NONE;

		CtgPositionData positionData;
		if (!decode(pos, positionData))
			return MOVE_NONE;

		CtgMoveList ctgMoveList;
		get_moves(pos, positionData, ctgMoveList);

		if (ctgMoveList.size() == 0)
			return MOVE_NONE;

		//Remove red moves and any moves with negative weight
		ctgMoveList.erase(
			remove_if(
				ctgMoveList.begin(),
				ctgMoveList.end(),
				[&](const CtgMove& x)
				{
					return x.red() || (onlyGreen && !x.green()) || x.weight() < 0;
				}),
			ctgMoveList.end());

		//Sort moves accorging to their weights
		stable_sort(ctgMoveList.begin(), ctgMoveList.end(), [](const CtgMove& mv1, const CtgMove& mv2) { return mv1.weight() > mv2.weight(); });

		//Only keep the top 'width' moves in the list
		while (ctgMoveList.size() > width)
			ctgMoveList.pop_back();

		size_t selectedMoveIndex = 0;
		if (ctgMoveList.size() > 1)
		{
			//Although not needed, let's shuffle candidate book moves just in case the random engine is more biased towards the middle
			shuffle(ctgMoveList.begin(), ctgMoveList.end(), randomEngine);

			//Return a random move
			selectedMoveIndex = (randomEngine() - randomEngine.min()) % ctgMoveList.size();
		}

		return ctgMoveList[selectedMoveIndex].sf_move();
	}

	void CtgBook::show_moves(const Position& pos) const
	{
		stringstream ss;

		if (!is_open())
		{
			assert(false);
			ss << "No book loaded" << endl;
		}
		else
		{
			CtgPositionData positionData;
			if (!decode(pos, positionData))
			{
				ss << "Position not found in book" << endl;
			}
			else
			{
				CtgMoveList ctgMoveList;
				get_moves(pos, positionData, ctgMoveList);

				if (ctgMoveList.size() == 0)
				{
					ss << "No moves found for this position" << endl;
				}
				else
				{
					ss << "MOVE      WIN       DRAW      LOSS      WEIGHT" << endl;

					for (const CtgMove& m : ctgMoveList)
					{
						ss
							<< setw(10) << left << UCI::move(m.sf_move(), pos.is_chess960())
							<< setw(10) << left << m.win
							<< setw(10) << left << m.draw
							<< setw(10) << left << m.loss
							<< setw(10) << left << m.weight()
							<< endl;
					}
				}
			}
		}

		//Not using sync_cout/sync_endl
		cout << ss.str() << endl;
	}
}