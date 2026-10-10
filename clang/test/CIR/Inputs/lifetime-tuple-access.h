namespace std {
template <class T> T &&move(T &value) { return static_cast<T &&>(value); }
template <class A, class B> struct pair {
  A first;
  B second;
  pair(A, B) noexcept;
  pair(const pair &) noexcept;
  pair(pair &&) noexcept;
};
template <class... Types> struct tuple {
  tuple(Types...) noexcept;
  tuple(const tuple &) noexcept;
  tuple(tuple &&) noexcept;
  tuple &operator=(const tuple &) noexcept;
  template <class... Other> tuple &operator=(const tuple<Other...> &) noexcept;
};
template <class T> struct tuple_size;
template <class A, class B> struct tuple_size<pair<A, B>> {
  static constexpr unsigned value = 2;
};
template <class... Types> struct tuple_size<tuple<Types...>> {
  static constexpr unsigned value = sizeof...(Types);
};
template <class T> struct tuple_size<const T> : tuple_size<T> {};
template <unsigned I, class T> struct tuple_element;
template <class A, class B> struct tuple_element<0, pair<A, B>> { using type = A; };
template <class A, class B> struct tuple_element<1, pair<A, B>> { using type = B; };
template <class First, class... Rest>
struct tuple_element<0, tuple<First, Rest...>> { using type = First; };
template <unsigned I, class First, class... Rest>
struct tuple_element<I, tuple<First, Rest...>>
    : tuple_element<I - 1, tuple<Rest...>> {};
template <unsigned I, class T> struct tuple_element<I, const T> {
  using type = const typename tuple_element<I, T>::type;
};
template <unsigned I, class T>
using tuple_element_t = typename tuple_element<I, T>::type;
template <unsigned I, class T> tuple_element_t<I, T> &get(T &) noexcept;
template <unsigned I, class T> const tuple_element_t<I, T> &get(const T &) noexcept;
template <unsigned I, class T> tuple_element_t<I, T> &&get(T &&) noexcept;
template <unsigned I, class T> const tuple_element_t<I, T> &&get(const T &&) noexcept;
template <class Element, class T> Element &get(T &) noexcept;
template <class Element, class T> const Element &get(const T &) noexcept;
template <class Element, class T> Element &&get(T &&) noexcept;
template <class Element, class T> const Element &&get(const T &&) noexcept;
template <class... Types> tuple<Types &...> tie(Types &...) noexcept;
}
