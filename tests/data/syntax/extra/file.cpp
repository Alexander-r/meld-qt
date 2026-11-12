#include <generator>
#include <memory>
#include <string_view>

namespace sample {

// Lines of a text, as a generator (see
// https://en.cppreference.com/w/cpp/coroutine/generator)
std::generator<std::string_view> lines(std::string_view text)
{
    while (!text.empty()) {
        const auto end = text.find('\n');
        co_yield text.substr(0, end);
        if (end == std::string_view::npos)
            co_return;
        text.remove_prefix(end + 1);
    }
}

struct Node {
    std::unique_ptr<Node> next = nullptr;
    [[nodiscard]] constexpr bool isLast() const noexcept { return next == nullptr; }
};

template <typename T>
auto count(const T &items) -> std::size_t
{
    std::size_t n = 0uL;
    for ([[maybe_unused]] const auto &item : items)
        ++n;
    return n * 1.5e0 > 0 ? n : 0x0;
}

} // namespace sample
