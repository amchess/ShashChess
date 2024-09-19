#ifdef USE_LIVEBOOK
    #include "Cp.h"

Cp::Cp(const int32_t new_score) :
    score(new_score) {}

Cp* Cp::opponent() const { return new Cp(-score); }

int32_t Cp::get_score() const { return score; }

Cp* Cp::flip() const { return new Cp(-score); }

bool Cp::operator==(const Cp& other) const { return score == other.score; }

bool Cp::operator!=(const Cp& other) const { return score != other.score; }

bool Cp::operator<(const Cp& other) const { return score < other.score; }

bool Cp::operator>(const Cp& other) const { return score > other.score; }

bool Cp::operator<=(const Cp& other) const { return score <= other.score; }

bool Cp::operator>=(const Cp& other) const { return score >= other.score; }
#endif
