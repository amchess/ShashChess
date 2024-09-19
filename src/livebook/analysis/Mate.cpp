#ifdef USE_LIVEBOOK
    #include "Mate.h"

Mate::Mate(const int32_t mate) :
    mate(mate) {}

Mate* Mate::opponent() const { return new Mate(-mate); }

int32_t Mate::get_mate() const { return mate; }

Mate* Mate::flip() const { return new Mate(-mate); }

bool Mate::operator==(const Mate& other) const { return mate == other.mate; }

bool Mate::operator!=(const Mate& other) const { return mate != other.mate; }

bool Mate::operator<(const Mate& other) const { return mate < other.mate; }

bool Mate::operator>(const Mate& other) const { return mate > other.mate; }

bool Mate::operator<=(const Mate& other) const { return mate <= other.mate; }

bool Mate::operator>=(const Mate& other) const { return mate >= other.mate; }
#endif
