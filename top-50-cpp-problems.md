# Top 50 C++ Coding Problems for Paper Interviews

**Target role:** C++ Developer on a Vision Systems team. This document goes with [top-100-questions.md](top-100-questions.md). Each problem lists the related question numbers (Q#) from that document.

## How to Use This Document

- **Try each problem on paper first**, with a 10–15 minute timer. Write the function signature, state your assumptions, dry-run an example, and then open the solution.
- Solutions are hidden in collapsible **Solution** blocks so you don't see them by accident.
- Most solutions are **5–15 lines of core logic**. Class-design problems (marked as such) need a few extra lines of declarations.
- Every solution compiles as C++20 and was checked against test cases, including edge cases.
- ⭐ means it is asked very often. The level markers are 🟢 Easy–Medium, 🟡 Medium, and 🟠 Medium–Hard.

| # | Section | Problems | What it exercises |
|---|---------|----------|-------------------|
| 1 | Fundamentals, Bits & Strings | P1–P5 | bit tricks, two pointers, overflow, `<cctype>` pitfalls |
| 2 | Pointers & Arrays | P6–P9 | pointer arithmetic, `void*`, `int*&` vs `int**`, in-place algorithms |
| 3 | Memory Management & RAII | P10–P12 | `new[]`/`delete[]`, smart pointer internals, RAII wrappers |
| 4 | Classes & OOP | P13–P20 | abstraction, encapsulation, polymorphism, Rule of Three, `static`, the diamond problem, factories |
| 5 | Operator Overloading & Move Semantics | P21–P23 | arithmetic and stream operators, move operations, custom iterators |
| 6 | Templates | P24–P26 | function and class templates, overloading vs specialization, variadic templates |
| 7 | STL, Lambdas & Modern C++ | P27–P29 | `unordered_map`, comparators, erase-remove, structured bindings |
| 8 | Sorting & Searching | P30–P35 | insertion, quick and merge sort, binary search variants, heaps |
| 9 | Linked Lists | P36–P39 | dummy nodes, fast/slow pointers, pointer-to-pointer |
| 10 | Stacks, Queues & Hashing | P40–P42 | `std::stack`, auxiliary stacks, hash maps |
| 11 | Trees & Graphs | P43–P45 | BST, BFS, flood fill / connected components |
| 12 | Matrix & Image Processing | P46–P48 | rotation, convolution, integral images |
| 13 | Concurrency | P49–P50 | data races, atomics, mutexes, condition variables |

---

## 1. Fundamentals, Bits & Strings (P1–P5)

### P1. Count Set Bits & Check Power of Two ⭐
**Level:** 🟢 · **Related:** Q12

Write `int countSetBits(unsigned int n)`, which returns the number of 1-bits in `n`, and `bool isPowerOfTwo(unsigned int n)`.

```text
countSetBits(13)  → 3        (13 = 0b1101)
isPowerOfTwo(64)  → true
isPowerOfTwo(0)   → false
```

**Edge cases:** `0`; all bits set (`0xFFFFFFFF`). Why does the parameter type matter? (Hint: think about signed integers.)

<details>
<summary><b>Solution</b></summary>

```cpp
int countSetBits(unsigned int n) {
    int count = 0;
    while (n) {
        n &= n - 1;          // clears the lowest set bit
        ++count;
    }
    return count;
}

bool isPowerOfTwo(unsigned int n) {
    return n != 0 && (n & (n - 1)) == 0;
}
```

**Why it works:** `n - 1` flips the lowest set bit and every bit below it, so `n & (n - 1)` removes exactly one set bit. A power of two has exactly one set bit.

**Complexity:** O(number of set bits) time, O(1) space.

**Follow-ups:**
- Use `std::popcount` / `std::has_single_bit` (C++20).
- Swap two ints with XOR. Why does XOR-swap break when both arguments refer to the same variable?

</details>

---

### P2. Reverse the Words in a Sentence (In Place) ⭐
**Level:** 🟡 · **Related:** Q72

Reverse the order of the words in a string **in place**. Words are separated by single spaces.

```text
"the sky is blue"  →  "blue is sky the"
```

**Edge cases:** an empty string; a single word.

<details>
<summary><b>Solution</b></summary>

```cpp
void reverseWords(std::string& s) {
    std::reverse(s.begin(), s.end());                  // "eulb si yks eht"
    size_t start = 0;
    for (size_t i = 0; i <= s.size(); ++i) {
        if (i == s.size() || s[i] == ' ') {
            std::reverse(s.begin() + start, s.begin() + i);   // fix each word
            start = i + 1;
        }
    }
}
```

**Why it works:** Reversing the whole string puts the words in the right order but spells each one backwards. Reversing each word again fixes the spelling.

**Complexity:** O(n) time, O(1) extra space.

**Follow-ups:**
- If `std::reverse` isn't allowed, write it with a two-pointer swap loop.
- Handle multiple, leading and trailing spaces.

</details>

---

### P3. Valid Palindrome (Ignoring Case and Punctuation)
**Level:** 🟢 · **Related:** Q12

Return `true` if the string reads the same forwards and backwards. Consider only letters and digits, and ignore case.

```text
"A man, a plan, a canal: Panama"  → true
"race a car"                      → false
```

**Edge cases:** an empty string; a string with no letters or digits; characters with negative `char` values.

<details>
<summary><b>Solution</b></summary>

```cpp
bool isPalindrome(const std::string& s) {
    auto uc = [](char c) { return static_cast<unsigned char>(c); };  // <cctype> needs non-negative values
    int l = 0, r = static_cast<int>(s.size()) - 1;
    while (l < r) {
        if (!std::isalnum(uc(s[l])))      ++l;
        else if (!std::isalnum(uc(s[r]))) --r;
        else {
            if (std::tolower(uc(s[l])) != std::tolower(uc(s[r]))) return false;
            ++l;
            --r;
        }
    }
    return true;
}
```

**Why it matters:** Passing a negative `char` to `std::isalnum` or `std::tolower` is **undefined behavior**, which is why each char is cast to `unsigned char` first. Few candidates know this, so mentioning it impresses interviewers.

**Complexity:** O(n) time, O(1) space.

</details>

---

### P4. Implement `atoi` (String to Integer) ⭐
**Level:** 🟡 · **Related:** Q12

Convert a string to an `int`, following these rules:
- Skip leading spaces.
- Read an optional `+` or `-` sign.
- Read digits until the first non-digit.
- Clamp the result to the `int` range if it overflows.

```text
"   -42"          → -42
"4193 with words" → 4193
"words 987"       → 0
"-91283472332"    → INT_MIN
```

**Edge cases:** an empty string, only a sign, and overflow in both directions.

<details>
<summary><b>Solution</b></summary>

```cpp
int myAtoi(const std::string& s) {
    size_t i = 0;
    while (i < s.size() && s[i] == ' ') ++i;                    // 1) skip leading spaces
    int sign = 1;
    if (i < s.size() && (s[i] == '+' || s[i] == '-'))           // 2) optional sign
        sign = (s[i++] == '-') ? -1 : 1;
    long long result = 0;
    while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) {
        result = result * 10 + (s[i++] - '0');                  // 3) accumulate digits
        if (sign * result > INT_MAX) return INT_MAX;            // 4) clamp on overflow
        if (sign * result < INT_MIN) return INT_MIN;
    }
    return static_cast<int>(sign * result);
}
```

**Why it works:** The `long long` accumulator can't overflow, because the function returns as soon as the value leaves the `int` range. `s[i] - '0'` converts a digit character to its value.

**Complexity:** O(n) time, O(1) space.

**Follow-up:** Do it without `long long`. Before multiplying, check whether `result > (INT_MAX - digit) / 10`.

</details>

---

### P5. Check If Two Strings Are Anagrams
**Level:** 🟢 · **Related:** Q71, Q77

Return `true` if `b` is a rearrangement of the characters in `a`.

```text
"listen", "silent" → true
"rat",    "car"    → false
```

<details>
<summary><b>Solution</b></summary>

```cpp
bool isAnagram(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    std::array<int, 256> count{};                 // one counter per possible byte value
    for (unsigned char c : a) ++count[c];
    for (unsigned char c : b)
        if (--count[c] < 0) return false;         // b has more of c than a
    return true;
}
```

**Complexity:** O(n) time, O(1) space (the table always has 256 entries).

**Alternative:** Sort both strings and compare them. That is O(n log n), and less efficient.

**Follow-up:** Find the first non-repeating character in a string, using the same counting idea in two passes.

</details>

---

## 2. Pointers & Arrays (P6–P9)

### P6. Implement `strlen`, `strcpy`, and `strcmp` Using Pointers ⭐
**Level:** 🟢 · **Related:** Q14, Q17

Implement these three C string functions using **pointer arithmetic only**, without array indexing.

<details>
<summary><b>Solution</b></summary>

```cpp
size_t myStrlen(const char* s) {
    const char* p = s;
    while (*p) ++p;
    return p - s;                          // pointer difference = number of chars
}

char* myStrcpy(char* dst, const char* src) {
    char* start = dst;
    while ((*dst++ = *src++)) {}           // copies the terminating '\0' too
    return start;                          // returned so calls can be chained
}

int myStrcmp(const char* a, const char* b) {
    while (*a && *a == *b) { ++a; ++b; }
    return static_cast<unsigned char>(*a) - static_cast<unsigned char>(*b);
}
```

**Talking points:**
- Why is `src` declared `const char*`?
- What happens if `dst` is too small? It's a buffer overflow, which is why `std::string` or `strncpy` is safer.
- Why compare characters as `unsigned char`?

</details>

---

### P7. Implement `memmove` (Overlap-Safe Copy)
**Level:** 🟡 · **Related:** Q17, Q18

Implement `void* myMemmove(void* dst, const void* src, size_t n)`. It must work correctly even when the source and destination regions **overlap**.

```text
buffer "abcdef":  myMemmove(buf + 2, buf, 4)  → "ababcd"
buffer "abcdef":  myMemmove(buf, buf + 2, 4)  → "cdefef"
```

<details>
<summary><b>Solution</b></summary>

```cpp
void* myMemmove(void* dst, const void* src, size_t n) {
    auto* d = static_cast<unsigned char*>(dst);
    auto* s = static_cast<const unsigned char*>(src);
    if (d == s || n == 0) return dst;
    if (d < s) {
        for (size_t i = 0; i < n; ++i) d[i] = s[i];          // copy forward
    } else {
        for (size_t i = n; i > 0; --i) d[i - 1] = s[i - 1];  // copy backward
    }
    return dst;
}
```

**Why it works:** If `dst` comes after `src`, copying forward would overwrite source bytes before they are read, so the function copies backward in that case. A `void*` can't be dereferenced, so it is cast to a byte pointer first.

**Follow-up:**
- How does this differ from `memcpy`? `memcpy` assumes the regions don't overlap, so it can be faster.
- Note: comparing pointers into *unrelated* objects with `<` gives an unspecified result; `std::less<>` guarantees a total order.

</details>

---

### P8. Allocate Memory for the Caller (`int*&` vs `int**`) ⭐
**Level:** 🟡 · **Related:** Q10, Q13

The function below is supposed to allocate an array for the caller. Explain why it fails. Then fix it in **two ways**: with a reference to a pointer and with a pointer to a pointer. Also write `swap` once with pointers and once with references.

```cpp
void allocate(int* p, size_t n) { p = new int[n]; }

int* data = nullptr;
allocate(data, 10);     // data is still nullptr!
```

<details>
<summary><b>Solution</b></summary>

```cpp
void allocateBroken(int* p, size_t n) { p = new int[n]; }     // BUG: changes a local copy, leaks

void allocateRef(int*& p, size_t n)  { p = new int[n](); }    // reference to pointer
void allocatePtr(int** pp, size_t n) { *pp = new int[n](); }  // pointer to pointer

void swapPtr(int* a, int* b) { int t = *a; *a = *b; *b = t; }
void swapRef(int& a, int& b) { int t = a; a = b; b = t; }
```

Usage: `allocateRef(data, 10);` or `allocatePtr(&data, 10);`.

**Why the original fails:** Pointers are passed **by value**. The function changes its own copy of the pointer, so the caller's `data` stays `nullptr` and the allocated memory leaks.

**Modern answer:** Return the result instead, for example `std::vector<int>` or `std::unique_ptr<int[]>`.

</details>

---

### P9. Rotate an Array by k Positions In Place
**Level:** 🟡 · **Related:** Q17

Rotate an array to the right by `k` positions, using O(1) extra space. A negative `k` rotates to the left.

```text
[1 2 3 4 5], k = 2  → [4 5 1 2 3]
[1 2 3 4 5], k = 7  → [4 5 1 2 3]   (7 % 5 = 2)
[1 2 3 4 5], k = -1 → [2 3 4 5 1]
```

<details>
<summary><b>Solution</b></summary>

```cpp
void reverseRange(int* first, int* last) {         // reverses [first, last)
    while (first < last) std::swap(*first++, *--last);
}

void rotateRight(int* arr, int n, int k) {
    if (n <= 0) return;
    k = ((k % n) + n) % n;                 // normalize; negative k rotates left
    reverseRange(arr, arr + n);            // [1 2 3 4 5] -> [5 4 3 2 1]
    reverseRange(arr, arr + k);            //             -> [4 5 3 2 1]
    reverseRange(arr + k, arr + n);        //             -> [4 5 1 2 3]
}
```

**Why it works:** The reversal trick takes three passes, so it is O(n) time and O(1) space. The half-open range `[first, last)` never forms a pointer before the start of the array, which would be UB.

**Follow-up:** `std::rotate` does the same thing. A simpler approach copies into a temporary array, but that needs O(n) extra space.

</details>

---

## 3. Memory Management & RAII (P10–P12)

### P10. Allocate and Free a Dynamic 2D Array
**Level:** 🟡 · **Related:** Q24, Q26, Q82

Allocate a `rows × cols` matrix of `int` in two ways:
- **(a)** with `new`, as an array of row pointers. Write the matching cleanup.
- **(b)** as **one contiguous block**.

Which layout is better for image data, and why?

<details>
<summary><b>Solution</b></summary>

```cpp
// (a) Array of row pointers: rows+1 allocations, rows may be scattered in memory
int** alloc2D(int rows, int cols) {
    int** m = new int*[rows];
    for (int r = 0; r < rows; ++r) m[r] = new int[cols]();   // () zero-initializes
    return m;
}

void free2D(int** m, int rows) {
    for (int r = 0; r < rows; ++r) delete[] m[r];   // free each row first...
    delete[] m;                                     // ...then the array of pointers
}

// (b) One contiguous block: one allocation, cache-friendly, how images are stored
struct Grid {
    int rows, cols;
    std::vector<int> data;
    Grid(int r, int c) : rows(r), cols(c), data(static_cast<size_t>(r) * c) {}
    int& at(int r, int c) { return data[static_cast<size_t>(r) * cols + c]; }
};
```

**Talking points:**
- Layout (b) makes one allocation, is cache-friendly, frees itself through RAII, and can be passed straight to C APIs as a single pointer. It is also how `cv::Mat` stores pixels.
- In (a), if one of the row allocations throws, the rows already allocated **leak**. That is a good point to raise in favor of RAII.

</details>

---

### P11. Implement a Simplified `unique_ptr` ⭐
**Level:** 🟠 · **Class design** · **Related:** Q21, Q57

Implement `UniquePtr<T>` with:
- a constructor that takes a raw pointer, and a destructor;
- **no copying**, but move construction and move assignment;
- `operator*`, `operator->` and `get()`.

<details>
<summary><b>Solution</b></summary>

```cpp
template <typename T>
class UniquePtr {
public:
    explicit UniquePtr(T* p = nullptr) : ptr_(p) {}
    ~UniquePtr() { delete ptr_; }

    UniquePtr(const UniquePtr&) = delete;                 // exclusive ownership: no copies
    UniquePtr& operator=(const UniquePtr&) = delete;

    UniquePtr(UniquePtr&& other) noexcept : ptr_(std::exchange(other.ptr_, nullptr)) {}
    UniquePtr& operator=(UniquePtr&& other) noexcept {
        if (this != &other) {
            delete ptr_;                                  // release current object
            ptr_ = std::exchange(other.ptr_, nullptr);    // take ownership
        }
        return *this;
    }

    T& operator*() const  { return *ptr_; }
    T* operator->() const { return ptr_; }
    T* get() const        { return ptr_; }
private:
    T* ptr_;
};
```

**Talking points:**
- Why is the constructor `explicit`? It stops `UniquePtr<int> p = rawPtr;` from compiling silently.
- Why mark the moves `noexcept`?
- `std::exchange` returns the old value and stores the new one in a single step.

**Follow-ups:** Add `release()`, `reset()`, and a custom deleter template parameter.

</details>

---

### P12. RAII Wrapper for a C `FILE*`
**Level:** 🟡 · **Class design** · **Related:** Q27, Q38

Wrap C's `FILE*` in an RAII class `File` with these rules:
- Open the file in the constructor and **throw** if that fails.
- Close it in the destructor.
- Make the class **non-copyable** but movable.
- Provide a `write` method.

<details>
<summary><b>Solution</b></summary>

```cpp
class File {
public:
    File(const char* path, const char* mode) : fp_(std::fopen(path, mode)) {
        if (!fp_) throw std::runtime_error(std::string("Cannot open ") + path);
    }
    ~File() { if (fp_) std::fclose(fp_); }

    File(const File&) = delete;                       // two owners would fclose twice
    File& operator=(const File&) = delete;
    File(File&& other) noexcept : fp_(std::exchange(other.fp_, nullptr)) {}

    void write(const std::string& text) { std::fputs(text.c_str(), fp_); }
private:
    std::FILE* fp_;
};
```

**Why it works:** If anything throws after the `File` is constructed, stack unwinding still runs the destructor, so the file is always closed. The same pattern applies to camera handles, GPU buffers, sockets and mutexes.

**Alternative:** `std::unique_ptr<FILE, decltype(&std::fclose)> f(std::fopen(path, "r"), &std::fclose);`

</details>

---

## 4. Classes & OOP (P13–P20)

### P13. Shape Hierarchy with Runtime Polymorphism ⭐
**Level:** 🟢 · **Class design** · **Related:** Q43, Q48, Q49

Design an abstract `Shape` with `area()` and `name()`, and two concrete classes, `Circle` and `Rectangle`. Then write `double totalArea(...)` for a collection of mixed shapes.

<details>
<summary><b>Solution</b></summary>

```cpp
class Shape {
public:
    virtual ~Shape() = default;                  // polymorphic base → virtual destructor
    virtual double area() const = 0;             // pure virtual → Shape is abstract
    virtual std::string name() const = 0;
};

class Circle : public Shape {
public:
    explicit Circle(double r) : r_(r) {}
    double area() const override { return std::numbers::pi * r_ * r_; }
    std::string name() const override { return "Circle"; }
private:
    double r_;
};

class Rectangle : public Shape {
public:
    Rectangle(double w, double h) : w_(w), h_(h) {}
    double area() const override { return w_ * h_; }
    std::string name() const override { return "Rectangle"; }
private:
    double w_, h_;
};

double totalArea(const std::vector<std::unique_ptr<Shape>>& shapes) {
    double sum = 0;
    for (const auto& s : shapes) sum += s->area();   // dynamic dispatch
    return sum;
}
```

**Checklist the interviewer looks for:**
- a virtual destructor;
- pure virtual functions, which make `Shape` abstract;
- `override` and `const` on the overrides;
- private data (encapsulation);
- `vector<unique_ptr<Shape>>` rather than `vector<Shape>`, which would **slice** the objects.

</details>

---

### P14. Count Live Instances with a Static Member
**Level:** 🟡 · **Class design** · **Related:** Q6, Q34

Write a class `Widget` that tracks how many instances are **currently alive**. Then predict the count after each line:

```cpp
{
    Widget a;                   // ?
    Widget b = a;               // ?
    std::vector<Widget> v(3);   // ?
    b = a;                      // ?
}                               // ?
```

<details>
<summary><b>Solution</b></summary>

```cpp
class Widget {
public:
    Widget()                  { ++alive_; }
    Widget(const Widget&)     { ++alive_; }      // easy to forget: copies are new objects too
    Widget(Widget&&) noexcept { ++alive_; }
    ~Widget()                 { --alive_; }
    Widget& operator=(const Widget&) = default;  // assignment creates no new object

    static int alive() { return alive_; }
private:
    static inline int alive_ = 0;                // C++17; before: define `int Widget::alive_ = 0;` in a .cpp
};
```

**Answers:** 1 → 2 → 5 → 5 (assignment creates nothing) → 0.

**Classic bug:** If you only increment in the default constructor, copies are never counted, but every destructor still decrements, so the count goes **negative**.

**Follow-up:** Make it thread-safe by using `static inline std::atomic<int> alive_{0};`.

</details>

---

### P15. String Class with Deep Copy (Rule of Three) ⭐
**Level:** 🟠 · **Class design** · **Related:** Q29, Q33, Q34, Q35

Implement `MyString`, which owns a heap-allocated `char*`. Write the constructor, copy constructor, copy assignment operator and destructor so that copies are **independent** (deep copies).

<details>
<summary><b>Solution</b></summary>

```cpp
class MyString {
public:
    MyString(const char* s = "") : size_(std::strlen(s)), data_(new char[size_ + 1]) {
        std::memcpy(data_, s, size_ + 1);
    }
    MyString(const MyString& other) : MyString(other.data_) {}   // delegate → deep copy

    MyString& operator=(const MyString& other) {
        if (this != &other) {                                   // self-assignment guard
            char* fresh = new char[other.size_ + 1];            // allocate first: if new throws,
            std::memcpy(fresh, other.data_, other.size_ + 1);   // *this is still unchanged
            delete[] data_;
            data_ = fresh;
            size_ = other.size_;
        }
        return *this;
    }
    ~MyString() { delete[] data_; }

    const char* c_str() const { return data_; }
    size_t size() const { return size_; }
private:
    size_t size_;      // declared before data_ on purpose: members initialize in this order
    char*  data_;
};
```

**The traps this tests:**
1. **Without a copy constructor**, two objects share one buffer, which leads to a double `delete[]`.
2. **Without a self-assignment guard**, `s = s` deletes its own data before copying it.
3. **The order of member declarations matters.** `data_(new char[size_ + 1])` relies on `size_` being declared, and therefore initialized, first.
4. **Allocating before deleting** gives the strong exception guarantee.

**Follow-up:** Add move operations (Rule of Five) or rewrite the assignment with copy-and-swap (Q56).

</details>

---

### P16. Bank Account: Encapsulation + Custom Exceptions
**Level:** 🟢 · **Class design** · **Related:** Q43, Q93

Design a `BankAccount` class with `deposit`, `withdraw` and `balance`:
- An invalid amount throws `std::invalid_argument`.
- An overdraft throws a **custom** `InsufficientFunds` exception derived from `std::runtime_error`.

Show how a caller handles both.

<details>
<summary><b>Solution</b></summary>

```cpp
class InsufficientFunds : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;            // inherit the constructors
};

class BankAccount {
public:
    void deposit(long long cents) {
        if (cents <= 0) throw std::invalid_argument("amount must be positive");
        balance_ += cents;
    }
    void withdraw(long long cents) {
        if (cents <= 0) throw std::invalid_argument("amount must be positive");
        if (cents > balance_) throw InsufficientFunds("balance too low");
        balance_ -= cents;
    }
    long long balance() const { return balance_; }      // read-only access to state
private:
    long long balance_ = 0;                             // cents: never use double for money
};
```

Caller:

```cpp
try {
    account.withdraw(1'000'000);
} catch (const InsufficientFunds& e) {      // most specific handler first
    std::cerr << "Declined: " << e.what() << '\n';
} catch (const std::exception& e) {         // then the general one
    std::cerr << "Error: " << e.what() << '\n';
}
```

**Talking points:**
- There is no `setBalance()`, so the invariant can only change through validated operations.
- Catch by `const&` to avoid slicing the exception.
- Put handlers in order from most-derived to least-derived.
- Floating point is the wrong type for money.

</details>

---

### P17. Deep-Copy a Polymorphic Collection (Virtual `clone`)
**Level:** 🟡 · **Related:** Q42, Q53

Building on P13, write `deepCopy`, which returns an **independent copy** of a `std::vector<std::unique_ptr<Shape>>`. Why can't you just write `std::make_unique<Shape>(*s)`?

<details>
<summary><b>Solution</b></summary>

```cpp
class Shape {
public:
    virtual ~Shape() = default;
    virtual double area() const = 0;
    virtual std::unique_ptr<Shape> clone() const = 0;    // "virtual copy constructor"
};

class Circle : public Shape {
public:
    explicit Circle(double r) : r_(r) {}
    double area() const override { return std::numbers::pi * r_ * r_; }
    std::unique_ptr<Shape> clone() const override {
        return std::make_unique<Circle>(*this);          // copy ctor of the *dynamic* type
    }
private:
    double r_;
};
// Rectangle::clone() is identical: return std::make_unique<Rectangle>(*this);

std::vector<std::unique_ptr<Shape>> deepCopy(const std::vector<std::unique_ptr<Shape>>& src) {
    std::vector<std::unique_ptr<Shape>> out;
    out.reserve(src.size());
    for (const auto& s : src) out.push_back(s->clone());
    return out;
}
```

**Why `make_unique<Shape>(*s)` fails:**
- `Shape` is abstract, so it can't be instantiated.
- Even if it weren't abstract, the copy would be **sliced**, because only the derived class knows its own type.
- `clone()` is the "virtual constructor" idiom: virtual dispatch picks the right copy constructor.

</details>

---

### P18. Find and Fix the Bugs (Virtual Destructor, Hiding, Slicing) ⭐
**Level:** 🟡 · **Related:** Q49, Q51, Q52, Q53

The code below is meant to print `File: start` and then `File: hello`. Instead, it typically prints `Base: start`, `Base: hello`, `~Logger`, `~Logger`, and it leaks memory. **Find at least three bugs and fix them.**

```cpp
class Logger {
public:
    ~Logger() { std::cout << "~Logger\n"; }
    virtual void log(std::string msg) { std::cout << "Base: " << msg << "\n"; }
};

class FileLogger : public Logger {
public:
    FileLogger() : buffer_(new char[4096]) {}
    ~FileLogger() { delete[] buffer_; }
    void log(std::string msg) const { std::cout << "File: " << msg << "\n"; }
private:
    char* buffer_;
};

void useLogger(Logger logger) { logger.log("hello"); }

int main() {
    Logger* l = new FileLogger();
    l->log("start");
    useLogger(*l);
    delete l;
}
```

<details>
<summary><b>Solution</b></summary>

**Bugs:**
1. **`~Logger` is not virtual.** `delete l` through a base pointer is UB, `~FileLogger` never runs, and `buffer_` leaks.
2. **`FileLogger::log` is `const` but the base version isn't**, so it doesn't override anything; it *hides* the base version. Adding `override` would have caught this at compile time.
3. **`useLogger` takes a `Logger` by value**, so the object is **sliced** and `Logger::log` always runs. The copy's destructor also explains the second `~Logger` in the output.
4. **(Bonus)** `FileLogger` owns a raw buffer but has the default copy operations, which violates the Rule of Three. A copy would lead to a double `delete[]`.

```cpp
class Logger {
public:
    virtual ~Logger() = default;                                     // fix 1
    virtual void log(const std::string& msg) { std::cout << "Base: " << msg << "\n"; }
};

class FileLogger : public Logger {
public:
    void log(const std::string& msg) override {                      // fix 2
        std::cout << "File: " << msg << "\n";
    }
private:
    std::vector<char> buffer_ = std::vector<char>(4096);             // fix 4: Rule of Zero
};

void useLogger(Logger& logger) { logger.log("hello"); }             // fix 3

int main() {
    std::unique_ptr<Logger> l = std::make_unique<FileLogger>();     // no naked new/delete
    l->log("start");
    useLogger(*l);
}
```

**Tip:** With `-Wall`, Clang warns that `FileLogger::log` "hides overloaded virtual function". In an interview, mention that you would compile with warnings enabled.

</details>

---

### P19. The Diamond Problem with Virtual Inheritance
**Level:** 🟠 · **Related:** Q50, Q54

Model a `SmartCamera` that is both a `Camera` and a `Sensor`. Both of those derive from `Device`, which has an `id` and **no default constructor**. A `SmartCamera` must contain **exactly one** `Device`. Write the classes, then answer:
1. What does `SmartCamera sc;` print, and what is `sc.id`?
2. What happens if you remove `virtual`?
3. What happens if `SmartCamera` doesn't call `Device(...)`?

<details>
<summary><b>Solution</b></summary>

```cpp
struct Device {
    explicit Device(const std::string& id) : id(id) { std::cout << "Device(" << id << ") "; }
    std::string id;
};

struct Camera : virtual Device {
    Camera() : Device("cam") { std::cout << "Camera "; }
};

struct Sensor : virtual Device {
    Sensor() : Device("sensor") { std::cout << "Sensor "; }
};

struct SmartCamera : Camera, Sensor {
    SmartCamera() : Device("smart") { std::cout << "SmartCamera "; }   // most-derived builds Device
};
```

**Answers:**
1. It prints `Device(smart) Camera Sensor SmartCamera`, and `sc.id == "smart"`. The virtual base is constructed **first, and only once, by the most-derived class**. The `Device("cam")` and `Device("sensor")` initializers are **ignored**.
2. Without `virtual`, there are two `Device` subobjects, so `sc.id` is **ambiguous** and won't compile.
3. If `SmartCamera` doesn't call `Device(...)`, the compiler tries `Device()`, which doesn't exist, so it's a compile error.

</details>

---

### P20. Factory for Image Filters (Open/Closed Principle)
**Level:** 🟡 · **Class design** · **Related:** Q95, Q97

Write `createFilter(name)`, which returns a `std::unique_ptr<Filter>` for `"blur"` or `"sharpen"` and throws for unknown names. It should be possible to add a new filter **without changing any `if`/`switch` logic**.

<details>
<summary><b>Solution</b></summary>

```cpp
class Filter {
public:
    virtual ~Filter() = default;
    virtual std::string name() const = 0;
};
class BlurFilter : public Filter {
public:
    std::string name() const override { return "blur"; }
};
class SharpenFilter : public Filter {
public:
    std::string name() const override { return "sharpen"; }
};

std::unique_ptr<Filter> createFilter(const std::string& type) {
    using Creator = std::function<std::unique_ptr<Filter>()>;
    static const std::unordered_map<std::string, Creator> registry = {
        {"blur",    [] { return std::make_unique<BlurFilter>(); }},
        {"sharpen", [] { return std::make_unique<SharpenFilter>(); }},
    };
    auto it = registry.find(type);
    if (it == registry.end()) throw std::invalid_argument("Unknown filter: " + type);
    return it->second();
}
```

**Talking points:**
- Callers depend only on the `Filter` interface (dependency inversion), and adding a filter is one new registry line.
- The `static` local is built once, and its initialization is thread-safe.

**Follow-up:** Let filters register themselves (a static registrar object in each filter's .cpp) so the factory function never changes.

</details>

---

## 5. Operator Overloading & Move Semantics (P21–P23)

### P21. 2D Vector Class with Arithmetic and Stream Operators ⭐
**Level:** 🟢 · **Related:** Q55

Implement `Vec2` so that all of these work: `a + b`, `a += b`, `v * 2.0`, `2.0 * v`, `a == b`, and `std::cout << v` (which prints `(x, y)`).

<details>
<summary><b>Solution</b></summary>

```cpp
struct Vec2 {
    double x = 0, y = 0;

    Vec2& operator+=(const Vec2& o) { x += o.x; y += o.y; return *this; }
    Vec2& operator*=(double s)      { x *= s;   y *= s;   return *this; }
    bool operator==(const Vec2&) const = default;           // C++20: member-wise ==
};

Vec2 operator+(Vec2 a, const Vec2& b) { return a += b; }   // built on top of +=
Vec2 operator*(Vec2 v, double s)      { return v *= s; }
Vec2 operator*(double s, Vec2 v)      { return v *= s; }   // so that 2.0 * v also works

std::ostream& operator<<(std::ostream& os, const Vec2& v) {
    return os << '(' << v.x << ", " << v.y << ')';
}
```

**Talking points:**
- Compound operators are **members** and return `*this` by reference.
- Binary operators are **non-members**, so `2.0 * v` works. Implement them in terms of the compound operators.
- `<<` must be a non-member because its left operand is the stream.
- Comparing `double`s exactly with `==` is risky in real code; compare against an epsilon instead.

</details>

---

### P22. Implement Move Operations for a Buffer ⭐
**Level:** 🟡 · **Related:** Q57, Q58, Q59

Add a **move constructor** and **move assignment operator** to this class. Then say which special member function runs on each line of the usage code below.

```cpp
class Buffer {
public:
    explicit Buffer(size_t n) : size_(n), data_(new float[n]()) {}
    ~Buffer() { delete[] data_; }
    // TODO: move constructor and move assignment
private:
    size_t size_;
    float* data_;
};

Buffer a(100);
Buffer b = std::move(a);      // (1)
Buffer c(10);
c = std::move(b);             // (2)
Buffer d = makeBuffer();      // (3)  makeBuffer() returns Buffer(64)
Buffer e = c;                 // (4)
```

<details>
<summary><b>Solution</b></summary>

```cpp
class Buffer {
public:
    explicit Buffer(size_t n) : size_(n), data_(new float[n]()) {}
    ~Buffer() { delete[] data_; }

    Buffer(Buffer&& other) noexcept
        : size_(std::exchange(other.size_, 0)),
          data_(std::exchange(other.data_, nullptr)) {}       // steal, leave source empty

    Buffer& operator=(Buffer&& other) noexcept {
        if (this != &other) {
            delete[] data_;                                   // free what we own
            size_ = std::exchange(other.size_, 0);
            data_ = std::exchange(other.data_, nullptr);
        }
        return *this;
    }

    size_t size() const { return size_; }
private:
    size_t size_;
    float* data_;
};
```

**Answers:**
1. Move constructor.
2. Move assignment.
3. **Neither.** C++17 guarantees copy elision, so the object is built directly in `d`.
4. **Compile error.** Declaring move operations implicitly *deletes* the copy constructor.

**Key point:** After a move, the source must be left **valid**, with `nullptr` and size 0, so that its destructor is safe to run.

</details>

---

### P23. Make a Class Work in a Range-Based `for` Loop
**Level:** 🟡 · **Related:** Q55, Q65

Write a `Range` class so that `for (int i : Range(0, 5))` prints `0 1 2 3 4`. Implement both **prefix** and **postfix** `++` on its iterator, and explain the difference between them.

<details>
<summary><b>Solution</b></summary>

```cpp
class Range {
public:
    Range(int first, int last) : first_(first), last_(last) {}

    struct Iterator {
        int value;
        int operator*() const { return value; }
        Iterator& operator++() { ++value; return *this; }                       // prefix
        Iterator operator++(int) { Iterator old = *this; ++value; return old; } // postfix
        bool operator!=(const Iterator& o) const { return value != o.value; }
    };

    Iterator begin() const { return {first_}; }
    Iterator end() const   { return {last_}; }
private:
    int first_, last_;
};
```

**Talking points:**
- Range-for only needs `begin()` and `end()`, plus `*`, prefix `++` and `!=` on the iterator.
- The **dummy `int` parameter** is what marks the postfix version.
- Postfix returns a **copy of the old value**, so it is potentially more expensive. Prefer `++it` in loops.

</details>

---

## 6. Templates (P24–P26)

### P24. Generic `maxOf` and the C-String Trap
**Level:** 🟡 · **Related:** Q61, Q62

Write a function template `maxOf(a, b)`. Then explain what happens with:
1. `maxOf(3, 7.5)`
2. `maxOf(p, q)`, where `p` and `q` are `const char*`

Fix the second case.

<details>
<summary><b>Solution</b></summary>

```cpp
template <typename T>
const T& maxOf(const T& a, const T& b) {
    return (a < b) ? b : a;
}

// Overload for C-strings: compare contents, not addresses
const char* maxOf(const char* a, const char* b) {
    return (std::strcmp(a, b) < 0) ? b : a;
}
```

**Answers:**
1. **Compile error.** `T` deduces as both `int` and `double`, which conflict. Fix it with `maxOf<double>(3, 7.5)`, or use two type parameters with `std::common_type_t`.
2. The template compares **pointer addresses**, not text, so the result is effectively random. A plain overload is preferred over the template, which fixes it. Prefer **overloading** over function template specialization, which has an awkward signature (`template<> const char* const& maxOf<const char*>(const char* const&, const char* const&)`) and surprising overload-resolution behavior.

**Extra trap:** `const auto& m = maxOf(std::string("a"), std::string("b"));` leaves a **dangling** reference to a temporary.

</details>

---

### P25. Fixed-Capacity Stack Class Template
**Level:** 🟡 · **Class design** · **Related:** Q61, Q93

Implement `FixedStack<T, N>` with `push`, `pop`, `top`, `empty` and `size`. It must use **no heap allocation**, and it should throw on overflow or underflow.

<details>
<summary><b>Solution</b></summary>

```cpp
template <typename T, std::size_t N>
class FixedStack {
public:
    void push(const T& value) {
        if (size_ == N) throw std::overflow_error("stack full");
        data_[size_++] = value;
    }
    void pop() {
        if (empty()) throw std::underflow_error("stack empty");
        --size_;
    }
    const T& top() const {
        if (empty()) throw std::underflow_error("stack empty");
        return data_[size_ - 1];
    }
    bool empty() const        { return size_ == 0; }
    std::size_t size() const  { return size_; }
private:
    std::array<T, N> data_{};      // N is a compile-time constant → no heap allocation
    std::size_t size_ = 0;
};
```

**Talking points:**
- `N` is a **non-type template parameter**.
- `std::array` requires `T` to be default-constructible. Placement new over raw aligned storage removes that requirement.
- `top()` returns a `const&` so it doesn't copy.
- *Vision angle:* useful in real-time code where heap allocation on the hot path is banned.

</details>

---

### P26. Variadic `sum` and `print`
**Level:** 🟡 · **Related:** Q63

Write:
- `sum(args...)`, which adds any number of arguments, once with a **C++17 fold expression** and once with **pre-C++17 recursion**;
- `print(args...)`, which prints its arguments separated by `", "`.

```text
sum(1, 2, 3)        → 6
sum(1, 2.5)         → 3.5
print(1, "two", 3.5) prints: 1, two, 3.5
```

<details>
<summary><b>Solution</b></summary>

```cpp
// C++17 fold expression
template <typename... Args>
auto sum(Args... args) {
    return (args + ... + 0);            // binary fold; "+ 0" makes sum() with no args valid
}

// Pre-C++17: recursion + a base case
template <typename T>
T sumRec(T last) { return last; }

template <typename T, typename... Rest>
T sumRec(T first, Rest... rest) { return first + sumRec(rest...); }

// Print all arguments separated by ", "
template <typename First, typename... Rest>
void print(const First& first, const Rest&... rest) {
    std::cout << first;
    ((std::cout << ", " << rest), ...);  // comma-operator fold
    std::cout << '\n';
}
```

**Trap:** `sumRec(1, 2.5)` returns `int` 3, because the return type is the type of the **first** argument. The `auto` fold version correctly returns `3.5`.

</details>

---

## 7. STL, Lambdas & Modern C++ (P27–P29)

### P27. Top-K Most Frequent Words
**Level:** 🟡 · **Related:** Q69, Q84

Given a text, return the `k` most frequent words with their counts. Sort by count in descending order, and break ties alphabetically.

```text
"the cat and the dog and the bird", k = 3  →  [(the,3), (and,2), (bird,1)]
```

<details>
<summary><b>Solution</b></summary>

```cpp
std::vector<std::pair<std::string, int>> topKWords(const std::string& text, size_t k) {
    std::unordered_map<std::string, int> freq;
    std::istringstream in(text);
    for (std::string word; in >> word; ) ++freq[word];

    std::vector<std::pair<std::string, int>> words(freq.begin(), freq.end());
    std::sort(words.begin(), words.end(), [](const auto& a, const auto& b) {
        if (a.second != b.second) return a.second > b.second;   // higher count first
        return a.first < b.first;                               // tie → alphabetical
    });
    if (words.size() > k) words.resize(k);
    return words;
}
```

**Talking points:**
- `freq[word]` inserts 0 the first time a word is seen, which is exactly what is wanted here.
- The comparator **must be a strict weak ordering**. Using `>=` is **UB** in `std::sort`.
- Use `std::partial_sort` for O(n log k).

</details>

---

### P28. Remove Duplicates (Order-Preserving) + Erase-Remove Idiom
**Level:** 🟢 · **Related:** Q72

1. Remove duplicates from a `vector<int>`, **keeping the first occurrence** of each value in its original order.
2. Remove all negative numbers using the erase-remove idiom.
3. Remove duplicates when order doesn't matter.

```text
[3, 1, 3, 2, 1, 4] → [3, 1, 2, 4]
```

<details>
<summary><b>Solution</b></summary>

```cpp
void removeDuplicates(std::vector<int>& v) {
    std::unordered_set<int> seen;
    size_t write = 0;
    for (int x : v)
        if (seen.insert(x).second)       // .second is true only the first time x is seen
            v[write++] = x;
    v.resize(write);
}

void removeNegatives(std::vector<int>& v) {
    v.erase(std::remove_if(v.begin(), v.end(), [](int x) { return x < 0; }), v.end());
}

void sortedUnique(std::vector<int>& v) {
    std::sort(v.begin(), v.end());
    v.erase(std::unique(v.begin(), v.end()), v.end());
}
```

**Talking points:**
- `remove_if` and `unique` don't change the container's size. They only move the kept elements to the front and return the new logical end, which is why `erase` is still needed.
- C++20 has `std::erase_if(v, pred)`.

</details>

---

### P29. Merge Overlapping Intervals
**Level:** 🟡 · **Related:** Q72, Q83

Given a list of `[start, end]` intervals, merge every pair that overlaps.

```text
[[1,3], [8,10], [2,6], [15,18]]  →  [[1,6], [8,10], [15,18]]
[[1,4], [4,5]]                   →  [[1,5]]
```

<details>
<summary><b>Solution</b></summary>

```cpp
using Interval = std::pair<int, int>;

std::vector<Interval> mergeIntervals(std::vector<Interval> intervals) {
    std::sort(intervals.begin(), intervals.end());          // by start, then by end
    std::vector<Interval> merged;
    for (const auto& [start, end] : intervals) {
        if (!merged.empty() && start <= merged.back().second)
            merged.back().second = std::max(merged.back().second, end);   // overlap → extend
        else
            merged.push_back({start, end});                               // gap → new interval
    }
    return merged;
}
```

**Talking points:**
- Taking the parameter by value is deliberate: we sort it anyway, and callers can move into it.
- `std::pair` compares lexicographically, so no custom comparator is needed.
- The loop uses structured bindings (C++17).
- *Vision angle:* merging overlapping time segments or 1D spans, such as runs in run-length-encoded masks.

**Complexity:** O(n log n), dominated by the sort.

</details>

---

## 8. Sorting & Searching (P30–P35)

### P30. Insertion Sort
**Level:** 🟢 · **Related:** Q81

Implement insertion sort. Is it stable? When is it the best choice?

<details>
<summary><b>Solution</b></summary>

```cpp
void insertionSort(std::vector<int>& a) {
    for (size_t i = 1; i < a.size(); ++i) {
        int key = a[i];
        size_t j = i;
        while (j > 0 && a[j - 1] > key) {   // '>' (not '>=') keeps the sort stable
            a[j] = a[j - 1];                // shift larger elements one step right
            --j;
        }
        a[j] = key;                         // drop key into the gap
    }
}
```

**Complexity:** O(n²) in the worst case, but **O(n) when the input is already nearly sorted**. It uses O(1) space and is stable.

**Talking points:**
- `std::sort` uses insertion sort for small partitions (around 16 elements or fewer) because it has very low overhead.
- Using `size_t j` with `j > 0` avoids the classic bug of an unsigned `j >= 0` loop that never ends.

**Also know these simple sorts:**

| Sort | Best | Worst | Stable | Note |
|------|------|-------|--------|------|
| Bubble (with early exit) | O(n) | O(n²) | Yes | Mostly asked as a warm-up |
| Selection | O(n²) | O(n²) | No | Minimizes the number of swaps (n − 1) |
| Insertion | O(n) | O(n²) | Yes | Best for small or nearly sorted data |

</details>

---

### P31. Quicksort ⭐
**Level:** 🟡 · **Related:** Q81

Implement quicksort with the Lomuto partition scheme.

<details>
<summary><b>Solution</b></summary>

```cpp
int partition(std::vector<int>& a, int lo, int hi) {   // Lomuto scheme, pivot = a[hi]
    int pivot = a[hi];
    int i = lo;                                         // a[lo..i-1] holds values < pivot
    for (int j = lo; j < hi; ++j)
        if (a[j] < pivot) std::swap(a[i++], a[j]);
    std::swap(a[i], a[hi]);                             // pivot lands in its final position
    return i;
}

void quickSort(std::vector<int>& a, int lo, int hi) {   // sorts a[lo..hi] inclusive
    if (lo >= hi) return;
    int p = partition(a, lo, hi);
    quickSort(a, lo, p - 1);
    quickSort(a, p + 1, hi);
}
```

Call it as `quickSort(v, 0, static_cast<int>(v.size()) - 1);`.

**Complexity:** O(n log n) on average. It sorts in place and is **not stable**.

**Talking points:**
- The worst case is **O(n²)**, for example on already sorted input with a last-element pivot. Avoid it by choosing a random or median-of-three pivot.
- Recurse into the smaller side first to keep the stack depth at O(log n).
- Hoare partitioning does fewer swaps.
- `std::sort` is introsort, which switches to heapsort when the recursion gets too deep.

</details>

---

### P32. Merge Sort ⭐
**Level:** 🟡 · **Related:** Q81

Implement a stable merge sort.

<details>
<summary><b>Solution</b></summary>

```cpp
void mergeSort(std::vector<int>& a, size_t lo, size_t hi) {   // sorts [lo, hi)
    if (hi - lo < 2) return;
    size_t mid = lo + (hi - lo) / 2;
    mergeSort(a, lo, mid);
    mergeSort(a, mid, hi);

    std::vector<int> merged;
    merged.reserve(hi - lo);
    size_t i = lo, j = mid;
    while (i < mid && j < hi) merged.push_back(a[i] <= a[j] ? a[i++] : a[j++]);  // <= keeps it stable
    while (i < mid) merged.push_back(a[i++]);
    while (j < hi)  merged.push_back(a[j++]);
    std::copy(merged.begin(), merged.end(), a.begin() + lo);
}
```

Call it as `mergeSort(v, 0, v.size());`.

**Complexity:** O(n log n) in **every** case, with O(n) extra space. It is stable.

**Talking points:**
- Merge sort is the preferred sort for linked lists, where merging needs no extra memory.
- It also suits external sorting (data too large for RAM).
- The merge loop alone is a common standalone question: "merge two sorted arrays".

</details>

---

### P33. Binary Search + First Occurrence ⭐
**Level:** 🟢 · **Related:** Q81

Given a sorted array:
- **(a)** return the index of `target`, or -1;
- **(b)** return the index of the **first** occurrence of `target` when there are duplicates.

```text
[1, 2, 2, 2, 5], target 2 → (a) any of 1..3   (b) 1
```

<details>
<summary><b>Solution</b></summary>

```cpp
int binarySearch(const std::vector<int>& a, int target) {
    int lo = 0, hi = static_cast<int>(a.size()) - 1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;            // not (lo + hi) / 2: that can overflow
        if (a[mid] == target) return mid;
        if (a[mid] < target) lo = mid + 1;
        else                 hi = mid - 1;
    }
    return -1;
}

int firstOccurrence(const std::vector<int>& a, int target) {   // same idea as std::lower_bound
    int lo = 0, hi = static_cast<int>(a.size());               // search space is [lo, hi)
    while (lo < hi) {
        int mid = lo + (hi - lo) / 2;
        if (a[mid] < target) lo = mid + 1;
        else                 hi = mid;                         // a[mid] might be the first one
    }
    return (lo < static_cast<int>(a.size()) && a[lo] == target) ? lo : -1;
}
```

**Complexity:** O(log n).

**Talking points:**
- Choose either a closed interval `[lo, hi]` or a half-open interval `[lo, hi)`, and keep the loop condition and the updates consistent with it.
- **Follow-ups:**
  - Count the occurrences of a value: `upper_bound - lower_bound`.
  - Find the last occurrence.
  - Compute an integer square root by binary-searching the answer.

</details>

---

### P34. Search in a Rotated Sorted Array
**Level:** 🟠 · **Related:** Q81

A sorted array with distinct values has been rotated at an unknown pivot, for example `[0,1,2,4,5,6,7]` → `[4,5,6,7,0,1,2]`. Find `target` in **O(log n)**.

```text
[4,5,6,7,0,1,2], target 0 → 4
[4,5,6,7,0,1,2], target 3 → -1
```

<details>
<summary><b>Solution</b></summary>

```cpp
int searchRotated(const std::vector<int>& a, int target) {
    int lo = 0, hi = static_cast<int>(a.size()) - 1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        if (a[mid] == target) return mid;
        if (a[lo] <= a[mid]) {                                   // left half is sorted
            if (a[lo] <= target && target < a[mid]) hi = mid - 1;
            else                                    lo = mid + 1;
        } else {                                                 // right half is sorted
            if (a[mid] < target && target <= a[hi]) lo = mid + 1;
            else                                    hi = mid - 1;
        }
    }
    return -1;
}
```

**Why it works:** At least one half around `mid` is always sorted. If the target lies within that half's range, search there; otherwise, search the other half.

**Follow-up:** Find the rotation point, which is the index of the minimum element.

</details>

---

### P35. K-th Largest Element ⭐
**Level:** 🟡 · **Related:** Q70, Q79

Find the k-th largest element in an unsorted array, without sorting the whole array.

```text
[3, 2, 1, 5, 6, 4], k = 2 → 5
```

<details>
<summary><b>Solution</b></summary>

```cpp
int kthLargest(const std::vector<int>& nums, int k) {
    std::priority_queue<int, std::vector<int>, std::greater<int>> minHeap;  // holds the k largest so far
    for (int x : nums) {
        minHeap.push(x);
        if (static_cast<int>(minHeap.size()) > k) minHeap.pop();          // evict the smallest
    }
    return minHeap.top();                       // smallest of the k largest = k-th largest
}
```

**Complexity:** O(n log k) time, O(k) space. This works well on streaming data.

**Alternatives:**
- `std::nth_element(v.begin(), v.begin() + k - 1, v.end(), std::greater<>())` runs in O(n) on average. It uses quickselect.
- *Vision angle:* keeping the top-K keypoints or detections by score.

</details>

---

## 9. Linked Lists (P36–P39)

All problems in this section use this node type:

```cpp
struct Node {
    int val;
    Node* next = nullptr;
};
```

### P36. Merge Two Sorted Linked Lists ⭐
**Level:** 🟢 · **Related:** Q74

Merge two sorted lists into one sorted list by **relinking the existing nodes**, without allocating new ones.

```text
1→3→5  +  2→3→4→6  →  1→2→3→3→4→5→6
```

<details>
<summary><b>Solution</b></summary>

```cpp
Node* mergeSorted(Node* a, Node* b) {
    Node dummy{0};                   // placeholder head: no special case for the first node
    Node* tail = &dummy;
    while (a && b) {
        if (a->val <= b->val) { tail->next = a; a = a->next; }
        else                  { tail->next = b; b = b->next; }
        tail = tail->next;
    }
    tail->next = a ? a : b;          // attach whatever is left
    return dummy.next;
}
```

**Complexity:** O(n + m) time, O(1) space.

**Talking point:** The **dummy head node** removes the special case of choosing the first node. It lives on the stack, so it needs no cleanup.

</details>

---

### P37. Detect a Cycle and Find Where It Starts ⭐
**Level:** 🟡 · **Related:** Q75

Return the node where the cycle begins, or `nullptr` if the list has no cycle. Use **O(1) extra space**.

<details>
<summary><b>Solution</b></summary>

```cpp
Node* cycleStart(Node* head) {
    Node* slow = head;
    Node* fast = head;
    while (fast && fast->next) {
        slow = slow->next;                       // 1 step
        fast = fast->next->next;                 // 2 steps
        if (slow == fast) {                      // they met → there is a cycle
            slow = head;                         // restart one pointer from head
            while (slow != fast) { slow = slow->next; fast = fast->next; }
            return slow;                         // meeting point = cycle entry
        }
    }
    return nullptr;                              // fast hit the end → no cycle
}
```

**Why it works (Floyd's algorithm):**
- Let `a` be the distance from the head to the start of the cycle, `b` the distance from the cycle start to the meeting point, and `L` the cycle length.
- Fast travels twice as far as slow, so `2(a + b) = a + b + kL`. That gives `a = kL - b`.
- So a pointer starting at the head and a pointer starting at the meeting point, both moving one step at a time, meet exactly at the start of the cycle.

**Follow-up:** Find the middle node. When `fast` reaches the end, `slow` is at the middle.

</details>

---

### P38. Remove the N-th Node from the End (One Pass)
**Level:** 🟡 · **Related:** Q74

Remove the n-th node from the end of the list in a single pass, and free its memory. Assume `1 ≤ n ≤ length`.

```text
1→2→3→4→5, n = 2  →  1→2→3→5
```

<details>
<summary><b>Solution</b></summary>

```cpp
Node* removeNthFromEnd(Node* head, int n) {
    Node dummy{0, head};                                // makes "remove the head" a normal case
    Node* fast = &dummy;
    Node* slow = &dummy;
    for (int i = 0; i <= n; ++i) fast = fast->next;     // open a gap of n+1 nodes
    while (fast) { fast = fast->next; slow = slow->next; }
    Node* victim = slow->next;                          // slow is just before the target
    slow->next = victim->next;
    delete victim;
    return dummy.next;
}
```

**Edge cases tested:** removing the head (n = length), removing the tail (n = 1), and a single-node list, which becomes `nullptr`.

</details>

---

### P39. Delete All Nodes with a Value (Pointer-to-Pointer Technique)
**Level:** 🟡 · **Related:** Q15, Q74

Delete every node whose value equals `value`, **without a dummy node and without a special case for the head**.

```text
7→7→1→7→2→7, value 7  →  1→2
```

<details>
<summary><b>Solution</b></summary>

```cpp
void removeAll(Node*& head, int value) {
    Node** link = &head;                  // address of the pointer we may need to rewrite
    while (*link) {
        if ((*link)->val == value) {
            Node* victim = *link;
            *link = victim->next;         // unlink: works for the head too
            delete victim;
        } else {
            link = &(*link)->next;        // advance to the next link
        }
    }
}
```

**Why it works:** `link` points at *the pointer that points to the current node*. That pointer is either `head` or some node's `next`. Rewriting `*link` works the same way in both cases, so the head needs no special code. Linus Torvalds famously called this the "good taste" way to delete from a linked list.

</details>

---

## 10. Stacks, Queues & Hashing (P40–P42)

### P40. Balanced Brackets ⭐
**Level:** 🟢 · **Related:** Q76

Return `true` if every `(`, `[` and `{` is closed by the matching bracket in the correct order. Ignore all other characters.

```text
"{[()()]}" → true      "([)]" → false      "((" → false      "())" → false
```

<details>
<summary><b>Solution</b></summary>

```cpp
bool isBalanced(const std::string& s) {
    std::stack<char> open;
    for (char c : s) {
        if (c == '(' || c == '[' || c == '{') {
            open.push(c);
        } else if (c == ')' || c == ']' || c == '}') {
            if (open.empty()) return false;                 // closer with no opener
            char o = open.top();
            open.pop();
            if ((c == ')' && o != '(') || (c == ']' && o != '[') || (c == '}' && o != '{'))
                return false;                               // wrong kind of opener
        }
    }
    return open.empty();                                    // leftover openers → unbalanced
}
```

**Complexity:** O(n) time, O(n) space.

**Talking point:** All three failure modes are handled: a closer with no opener, a mismatched pair, and openers left over at the end.

</details>

---

### P41. Min Stack (`getMin` in O(1))
**Level:** 🟡 · **Class design** · **Related:** Q76

Design a stack that supports `push`, `pop`, `top` and `getMin`, all in **O(1)** time.

<details>
<summary><b>Solution</b></summary>

```cpp
class MinStack {
public:
    void push(int x) {
        values_.push(x);
        if (mins_.empty() || x <= mins_.top()) mins_.push(x);  // '<=' so duplicate minimums are kept
    }
    void pop() {
        if (values_.top() == mins_.top()) mins_.pop();
        values_.pop();
    }
    int top() const    { return values_.top(); }
    int getMin() const { return mins_.top(); }
private:
    std::stack<int> values_;
    std::stack<int> mins_;     // mins_.top() is always the current minimum
};
```

**Trap:** If you push onto `mins_` only when `x < min`, then pushing `3, 3` and popping once loses the minimum. That's why the condition is `<=`.

**Follow-up:** Implement a queue using two stacks (see Q76 in the companion document).

</details>

---

### P42. Two Sum ⭐
**Level:** 🟢 · **Related:** Q77

Return the indices of two numbers that add up to `target`, or nothing if no such pair exists. Do it in **O(n)**.

```text
[2, 7, 11, 15], target 9 → (0, 1)
```

<details>
<summary><b>Solution</b></summary>

```cpp
std::optional<std::pair<int, int>> twoSum(const std::vector<int>& nums, int target) {
    std::unordered_map<int, int> indexOf;                    // value → index seen so far
    for (int i = 0; i < static_cast<int>(nums.size()); ++i) {
        auto it = indexOf.find(target - nums[i]);           // have we seen the complement?
        if (it != indexOf.end()) return std::pair{it->second, i};
        indexOf[nums[i]] = i;
    }
    return std::nullopt;
}
```

**Complexity:** O(n) time, O(n) space.

**Talking points:**
- The function looks up the complement *before* inserting the current number, so an element is never paired with itself.
- `std::optional` makes the "no answer" case explicit.

**Follow-ups:**
- If the input is sorted, use two pointers with O(1) space.
- Longest substring without repeating characters is another hash-map problem, solved with a sliding window.

</details>

---

## 11. Trees & Graphs (P43–P45)

The tree problems use this node type:

```cpp
struct TreeNode {
    int val;
    TreeNode* left = nullptr;
    TreeNode* right = nullptr;
};
```

### P43. BST Insert + Validate a BST
**Level:** 🟡 · **Related:** Q78

1. Insert a value into a binary search tree.
2. Check whether a binary tree is a **valid** BST.

<details>
<summary><b>Solution</b></summary>

```cpp
TreeNode* insert(TreeNode* root, int value) {
    if (!root) return new TreeNode{value};
    if (value < root->val) root->left  = insert(root->left, value);
    else                   root->right = insert(root->right, value);
    return root;
}

bool isValidBST(const TreeNode* node, long long lo = LLONG_MIN, long long hi = LLONG_MAX) {
    if (!node) return true;
    if (node->val <= lo || node->val >= hi) return false;      // must lie strictly inside (lo, hi)
    return isValidBST(node->left, lo, node->val) &&            // left subtree: upper bound tightens
           isValidBST(node->right, node->val, hi);             // right subtree: lower bound tightens
}
```

**Classic wrong answer:** Checking only `left->val < node->val < right->val` misses the tree below, because the 3 is in the right subtree of 5 but is smaller than 5.

```text
      5
     / \
    4   6
       / \
      3   7
```

**Why `long long` bounds:** With `int` bounds, nodes holding `INT_MIN` or `INT_MAX` would be wrongly rejected.

**Alternative:** An in-order traversal must be strictly increasing.

</details>

---

### P44. Level-Order Traversal (BFS) ⭐
**Level:** 🟡 · **Related:** Q78, Q80

Return the values of a binary tree grouped by level.

```text
    3
   / \
  9   20        →  [[3], [9, 20], [15, 7]]
     /  \
    15   7
```

<details>
<summary><b>Solution</b></summary>

```cpp
std::vector<std::vector<int>> levelOrder(const TreeNode* root) {
    std::vector<std::vector<int>> levels;
    if (!root) return levels;
    std::queue<const TreeNode*> q;
    q.push(root);
    while (!q.empty()) {
        auto& level = levels.emplace_back();             // start a new level (C++17 returns ref)
        for (size_t n = q.size(); n > 0; --n) {          // exactly the nodes of this level
            const TreeNode* node = q.front();
            q.pop();
            level.push_back(node->val);
            if (node->left)  q.push(node->left);
            if (node->right) q.push(node->right);
        }
    }
    return levels;
}
```

**Key trick:** Take a snapshot of `q.size()` at the start of each level, so the loop processes exactly that level's nodes.

**Follow-ups:**
- Maximum depth of the tree (the number of levels).
- The right-side view.
- A zigzag-order traversal.

</details>

---

### P45. Count Connected Components in a Binary Image (Flood Fill)
**Level:** 🟡 · **Related:** Q80

Given a binary image, where `1` is foreground and `0` is background, count the separate foreground blobs using **4-connectivity**.

```text
1 1 0 0 0
1 1 0 0 1
0 0 0 1 1      →  5 components
0 1 0 0 0
1 0 0 1 1
```

<details>
<summary><b>Solution</b></summary>

```cpp
using Grid = std::vector<std::vector<int>>;

void floodFill(Grid& g, int r, int c) {
    if (r < 0 || c < 0 || r >= static_cast<int>(g.size()) || c >= static_cast<int>(g[r].size()))
        return;                                          // outside the image
    if (g[r][c] != 1) return;                            // background or already visited
    g[r][c] = 0;                                         // mark visited
    floodFill(g, r + 1, c);
    floodFill(g, r - 1, c);
    floodFill(g, r, c + 1);
    floodFill(g, r, c - 1);
}

int countComponents(Grid g) {                            // by value: caller's grid untouched
    int count = 0;
    for (int r = 0; r < static_cast<int>(g.size()); ++r)
        for (int c = 0; c < static_cast<int>(g[r].size()); ++c)
            if (g[r][c] == 1) { ++count; floodFill(g, r, c); }
    return count;
}
```

**Complexity:** O(rows × cols).

**Talking points:**
- In the example, the two diagonal 1s at the bottom left count as **separate** components under 4-connectivity. With 8-connectivity they would merge.
- **Production concern:** recursion can go as deep as the number of pixels, so a large blob will **overflow the stack**. In real code, use an explicit `std::stack` or `std::queue` (iterative DFS or BFS).

**Follow-ups:**
- Write a label id (2, 3, ...) instead of 0. That is connected-component labeling.
- Compute each blob's area or bounding box.

</details>

---

## 12. Matrix & Image Processing (P46–P48)

### P46. Rotate a Matrix / Image by 90° ⭐
**Level:** 🟡 · **Related:** Q82

1. Rotate an `n × n` matrix 90° clockwise **in place**.
2. Rotate a `W × H` grayscale image stored in a **1D row-major buffer**. This one doesn't need to be in place.

```text
1 2 3      7 4 1
4 5 6  →   8 5 2
7 8 9      9 6 3
```

<details>
<summary><b>Solution</b></summary>

```cpp
void rotate90Clockwise(std::vector<std::vector<int>>& m) {
    const size_t n = m.size();
    for (size_t i = 0; i < n; ++i)                 // 1) transpose across the main diagonal
        for (size_t j = i + 1; j < n; ++j)
            std::swap(m[i][j], m[j][i]);
    for (auto& row : m)                            // 2) mirror each row
        std::reverse(row.begin(), row.end());
}

// Follow-up: any W x H image stored in a 1D row-major buffer (out-of-place)
std::vector<uint8_t> rotate90(const std::vector<uint8_t>& src, int w, int h) {
    std::vector<uint8_t> dst(src.size());          // result is h wide, w tall
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            dst[x * h + (h - 1 - y)] = src[y * w + x];
    return dst;
}
```

**Talking points:**
- Transposing, then reversing each row, gives a **clockwise** rotation. Reversing first, or reversing columns, gives counter-clockwise.
- Note that `j` starts at `i + 1`. Starting at 0 would swap every pair twice and undo the transpose.
- In the 1D version, the output's width becomes `h`. The source is read sequentially, but the destination is written column by column, which is less cache-friendly. Blocked or tiled loops fix that.

</details>

---

### P47. 3×3 Box Blur on a Grayscale Image
**Level:** 🟡 · **Related:** Q82, Q98

Apply a 3×3 mean filter to an 8-bit grayscale image stored row-major in a `std::vector<uint8_t>`. At the borders, **replicate** the edge pixels.

<details>
<summary><b>Solution</b></summary>

```cpp
std::vector<uint8_t> boxBlur3x3(const std::vector<uint8_t>& src, int width, int height) {
    std::vector<uint8_t> dst(src.size());                  // never blur in place
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x) {
            int sum = 0;                                   // int: 9 * 255 doesn't fit in uint8_t
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx) {
                    int yy = std::clamp(y + dy, 0, height - 1);   // replicate border pixels
                    int xx = std::clamp(x + dx, 0, width - 1);
                    sum += src[yy * width + xx];
                }
            dst[y * width + x] = static_cast<uint8_t>((sum + 4) / 9);   // +4 rounds to nearest
        }
    return dst;
}
```

**What the interviewer checks:**
1. A **separate output buffer**. Blurring in place would read pixels that have already been blurred.
2. A **wider accumulator**, because a `uint8_t` sum overflows.
3. A **border policy**: replicate, zero, or reflect.
4. A **row-major loop order**, with `y` outside and `x` inside.

**Optimizations:**
- The box filter is **separable**: do a horizontal pass and then a vertical pass, which costs 6 reads per pixel instead of 9.
- A running sum makes the cost O(1) per pixel for any kernel size, as does an integral image (P48).
- SIMD and multithreading over rows.

</details>

---

### P48. Integral Image (Summed-Area Table)
**Level:** 🟠 · **Related:** Q82

Build an integral image so that the sum of the pixels in **any** rectangle can be computed in **O(1)**.

<details>
<summary><b>Solution</b></summary>

```cpp
// I is (w+1) x (h+1); I[y][x] = sum of all src pixels above and left of (x, y)
std::vector<long long> integralImage(const std::vector<uint8_t>& src, int w, int h) {
    const int W = w + 1;
    std::vector<long long> I(static_cast<size_t>(W) * (h + 1), 0);   // row 0 and col 0 stay 0
    for (int y = 1; y <= h; ++y)
        for (int x = 1; x <= w; ++x)
            I[y * W + x] = src[(y - 1) * w + (x - 1)]
                         + I[(y - 1) * W + x]          // above
                         + I[y * W + (x - 1)]          // left
                         - I[(y - 1) * W + (x - 1)];   // counted twice
    return I;
}

// Sum of pixels in the rectangle (x0, y0)..(x1, y1), inclusive, in O(1)
long long rectSum(const std::vector<long long>& I, int w, int x0, int y0, int x1, int y1) {
    const int W = w + 1;
    return I[(y1 + 1) * W + (x1 + 1)] - I[y0 * W + (x1 + 1)]
         - I[(y1 + 1) * W + x0]       + I[y0 * W + x0];
}
```

**Talking points:**
- The **extra zero row and column** remove all the boundary checks.
- A **64-bit** accumulator is needed, because a 4K image of 255s sums to about 2×10⁹, close to the limit of `int32`.
- Building the table is O(w·h), and each query costs 4 lookups.
- **Uses:** box filters of any size in O(1) per pixel, Haar features in Viola-Jones face detection, and adaptive thresholding.

</details>

---

## 13. Concurrency (P49–P50)

### P49. Fix the Data Race in a Shared Counter ⭐
**Level:** 🟢 · **Related:** Q88, Q91

This program usually prints **less than 400000**. Explain why, and fix it in **three** different ways.

```cpp
int counter = 0;
void work() { for (int i = 0; i < 100'000; ++i) ++counter; }

int main() {
    std::thread t1(work), t2(work), t3(work), t4(work);
    t1.join(); t2.join(); t3.join(); t4.join();
    std::cout << counter << '\n';
}
```

<details>
<summary><b>Solution</b></summary>

**Why:** `++counter` is actually three steps: **load, add, store**. Two threads can load the same value, and then one thread's increment overwrites the other's. Formally, this is a **data race**, which is undefined behavior.

```cpp
// Fix 1: std::atomic — simplest and best for a single counter
std::atomic<int> counter{0};
void work() {
    for (int i = 0; i < 100'000; ++i)
        counter.fetch_add(1, std::memory_order_relaxed);   // or simply ++counter;
}
```

```cpp
// Fix 2: mutex — needed when several variables must change together
int counter = 0;
std::mutex m;
void work() {
    for (int i = 0; i < 100'000; ++i) {
        std::lock_guard<std::mutex> lock(m);              // unlocks automatically each iteration
        ++counter;
    }
}
```

```cpp
// Fix 3: fastest — accumulate locally, publish once (counter is std::atomic<int>)
void work() {
    int local = 0;
    for (int i = 0; i < 100'000; ++i) ++local;            // no sharing in the hot loop
    counter += local;                                     // one synchronized update per thread
}
```

**Talking points:**
- `volatile` does **not** fix this.
- `memory_order_relaxed` is fine for a counter that is only read after `join()`.
- Fix 3 avoids contention and cache-line ping-pong. The same pattern applies to per-thread histograms in image processing.

</details>

---

### P50. Two Threads Print Odd and Even Numbers Alternately
**Level:** 🟠 · **Related:** Q87, Q90

Use two threads to print `1 2 3 ... n` in order. One thread prints only the odd numbers and the other only the even numbers. Synchronize them with a mutex and a condition variable.

<details>
<summary><b>Solution</b></summary>

```cpp
void printAlternately(int n) {
    std::mutex m;
    std::condition_variable cv;
    int current = 1;                                       // next number to print (shared)

    auto worker = [&](int parity) {                        // 1 = odd thread, 0 = even thread
        while (true) {
            std::unique_lock<std::mutex> lock(m);
            cv.wait(lock, [&] { return current > n || current % 2 == parity; });
            if (current > n) return;                       // done: exit the loop
            std::cout << current++ << ' ';
            cv.notify_all();                               // wake the other thread
        }
    };

    std::thread odd(worker, 1), even(worker, 0);
    odd.join();
    even.join();
}
```

**Talking points:**
- **`wait` with a predicate** handles both *spurious wakeups* and the case where a notification arrives before the other thread starts waiting.
- `condition_variable` requires a `unique_lock`; a `lock_guard` won't work.
- The `current > n` check in the predicate lets **both** threads exit cleanly. Forgetting it makes one thread wait forever, so `join()` hangs.
- Capturing locals by reference is safe here only because both threads are joined before the function returns (see Q84).

**Follow-up:** Generalize to N threads printing in round-robin order, where thread `i` prints when `current % N == i`.

</details>

---

## Paper-Coding Checklist

Use this checklist on every problem in a real interview:

1. **Clarify the problem.** Ask about the input size, duplicates, negative values, an empty input, and whether you may modify the input.
2. **Write the signature first.** Choose `const&` for read-only inputs, return by value, and use `std::optional` when there might be no answer.
3. **Talk through the approach and its complexity** before you write any code.
4. **Write clean code.** Use meaningful names, no magic numbers, and `size_t` versus `int` consistently.
5. **Dry-run a small example out loud.** Walk through the variables step by step.
6. **Check the edge cases:**
   - empty input, one element, or all elements equal;
   - `nullptr`;
   - integer overflow;
   - off-by-one errors in loop bounds;
   - image borders.
7. **Mention a production-quality improvement:**
   - RAII instead of `new`/`delete`;
   - an iterative version instead of deep recursion;
   - thread safety;
   - SIMD or cache-friendly memory access.
