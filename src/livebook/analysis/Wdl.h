#ifndef WDL_H
#define WDL_H
#ifdef USE_LIVEBOOK
    #include <cstdint>

class Wdl {
   public:
    Wdl(uint32_t wins, uint32_t draws, uint32_t losses);

    ~Wdl() = default;

    [[nodiscard]] Wdl* opponent() const;

    [[nodiscard]] uint32_t get_wins() const;
    [[nodiscard]] uint32_t get_draws() const;
    [[nodiscard]] uint32_t get_losses() const;

    [[nodiscard]] double get_win_probability() const;
    [[nodiscard]] double get_draw_probability() const;
    [[nodiscard]] double get_loss_probability() const;

    [[nodiscard]] double get_success_probability() const;

    [[nodiscard]] Wdl* flip() const;

    bool operator==(const Wdl& other) const;

    bool operator!=(const Wdl& other) const;

    bool operator<(const Wdl& other) const;

    bool operator>(const Wdl& other) const;

    bool operator<=(const Wdl& other) const;

    bool operator>=(const Wdl& other) const;

   protected:
    uint32_t wins;
    uint32_t draws;
    uint32_t losses;

    [[nodiscard]] uint32_t get_sum() const;
};

#endif  //USE_LIVEBOOK
#endif  //WDL_H
