# Top 100 C++ Interview Questions

**Target role:** C++ Developer on a Vision Systems team, where vision features and services are written in C++.

## How to Use This Document

- The questions are grouped by topic and run roughly from basics to advanced. The order follows what the technical test focused on: **C++ basics, pointers, classes and OOP, and common data structures**.
- Each question lists **Key points**, the minimum an interviewer expects to hear. Answer out loud first, then check yourself against them.
- ⭐ means it is asked very often, so know it cold. 💻 means you should be ready to write the code by hand.
- *Vision angle* notes show how to connect an answer to image processing. Doing that makes your answers stand out for this team.

| # | Topic | Questions |
|---|-------|-----------|
| 1 | C++ Fundamentals | Q1–Q12 |
| 2 | Pointers & References | Q13–Q23 |
| 3 | Memory Management | Q24–Q31 |
| 4 | Classes & Objects | Q32–Q42 |
| 5 | OOP: Inheritance & Polymorphism | Q43–Q54 |
| 6 | Operator Overloading & Move Semantics | Q55–Q60 |
| 7 | Templates & Generic Programming | Q61–Q64 |
| 8 | STL Containers & Algorithms | Q65–Q72 |
| 9 | Data Structures & Algorithms | Q73–Q82 |
| 10 | Modern C++ (C++11/14/17/20) | Q83–Q86 |
| 11 | Concurrency & Multithreading | Q87–Q92 |
| 12 | Exception Handling | Q93–Q94 |
| 13 | Design Principles & Patterns | Q95–Q97 |
| 14 | Vision Systems, Performance & Tooling | Q98–Q100 |

---

## 1. C++ Fundamentals (Q1–Q12)

### Q1. What are the main differences between C and C++? ⭐
**Key points:**
- C is procedural. C++ supports several paradigms: procedural, object-oriented, generic and functional.
- C++ adds classes, references, function and operator overloading, templates, exceptions, namespaces, `new`/`delete`, RAII and the STL.
- C++ has stricter type checking. For example, `int* p = malloc(4);` compiles in C but needs a cast in C++, and `sizeof('a')` is `sizeof(int)` in C but `1` in C++.

### Q2. What happens when you build a C++ program? ⭐
**Key points:**
- The steps are **preprocessing → compilation → assembly → linking**.
- The preprocessor expands `#include`, `#define` and `#if`, producing a *translation unit*. The compiler turns that into assembly and then an object file. The linker resolves symbols across object files and libraries.
- Know which stage reports which error: "undeclared identifier" comes from the compiler, while "undefined reference" or "unresolved external symbol" comes from the linker.
- Include guards and `#pragma once` stop a header from being included twice. Know the difference between static libraries (`.a`/`.lib`) and shared libraries (`.so`/`.dll`/`.dylib`).

### Q3. What is the difference between a declaration and a definition? What is the One Definition Rule (ODR)?
**Key points:**
- A declaration introduces a name and its type (`extern int x;`, `void f();`). A definition allocates storage or provides the body.
- A name can be declared many times, but a non-inline function or variable must be defined exactly once in the whole program.
- `inline` functions and variables, templates and class definitions may appear in several translation units as long as every copy is identical.

### Q4. Explain all the uses of `const` in C++. ⭐
**Key points:**
- **Const variables** are read-only after initialization.
- **Pointers:** a pointer to const and a const pointer are different things (see Q14).
- **`const T&` parameters** avoid copying large objects such as images, and they can bind to temporaries.
- **Const member functions** (`int width() const;`) cannot modify members, and they are the only functions you can call on a const object.
- Const-correctness is part of good API design. `mutable` is the deliberate exception (see Q39).

### Q5. What is the difference between `const`, `constexpr`, and `#define`? ⭐
**Key points:**
- `#define` is plain text substitution by the preprocessor. It has no type and no scope, and it is hard to debug.
- `const` makes a typed, read-only object. Its value may be computed at runtime.
- `constexpr` guarantees the value can be computed at compile time, so you can use it for array sizes and template arguments. A `constexpr` function can run at compile time or at runtime.
- Prefer `constexpr`/`const` over macros. C++20 adds `consteval` (must run at compile time) and `constinit`.

### Q6. What does the `static` keyword mean in different contexts? ⭐
**Key points:**
- **Static local variable:** initialized once (thread-safe since C++11) and lives until the program ends.
- **Static at namespace or file scope:** gives the name *internal linkage*, so it is visible only in that file. An unnamed namespace is the modern alternative.
- **Static data member:** one copy shared by all instances. It must be defined once, or declared `inline static` in C++17.
- **Static member function:** has no `this` pointer, can only use static members directly, and is called as `Class::f()`.

### Q7. What is the difference between `struct` and `class`?
**Key points:**
- The only difference is the default access level. Members and base classes of a `struct` are `public` by default; for a `class` they are `private`.
- Otherwise the two are identical: both can have methods, constructors, inheritance and virtual functions.
- By convention, use `struct` for plain data aggregates (`Point`, `Pixel`) and `class` for types that protect invariants.

### Q8. Explain the four C++ cast operators. Why prefer them over C-style casts? ⭐
**Key points:**
- **`static_cast`:** conversions checked at compile time, such as numeric conversions, upcasts, unchecked downcasts and `void*` → `T*`.
- **`dynamic_cast`:** a downcast checked at runtime in a polymorphic hierarchy. It returns `nullptr` for pointers or throws `std::bad_cast` for references. It needs RTTI and at least one virtual function.
- **`const_cast`:** adds or removes `const`. Modifying an object that was originally const is undefined behavior.
- **`reinterpret_cast`:** reinterprets the bits, for example viewing a raw byte buffer as another type. It is low-level and dangerous.
- C-style casts silently try these in sequence, can perform dangerous conversions without warning, and are hard to grep for.

### Q9. What are lvalues and rvalues (and xvalues/prvalues)?
**Key points:**
- An **lvalue** has an identity and an address, like a named variable.
- A **prvalue** is a pure temporary value: `42`, `a + b`, or the result of a function that returns by value.
- An **xvalue** is an "expiring" value, such as the result of `std::move(x)`.
- These categories decide which overload is chosen (`T&`, `const T&` or `T&&`), and they are the foundation of move semantics.

### Q10. What are the differences between pass-by-value, pass-by-pointer, and pass-by-reference? When do you use each? ⭐
**Key points:**
- **By value:** for cheap types, or for "sink" parameters you will store anyway, where you take by value and then `std::move`.
- **`const T&`:** for large read-only objects such as `const Image&`.
- **`T&`:** for output parameters that must exist.
- **Pointer:** when "no object" (`nullptr`) is a valid input, or when talking to C APIs.
- Smart pointer parameters express ownership. Pass `unique_ptr` by value to transfer ownership. Pass a `shared_ptr` only when the callee will share ownership; otherwise pass `T&`.

### Q11. What is function overloading? How does the compiler resolve overloads? Can you overload on return type alone?
**Key points:**
- Overloads share a name but have different parameter lists: different number of parameters, types, or `const`/ref qualifiers on member functions.
- Resolution ranks the candidates: exact match > promotion > standard conversion > user-defined conversion > ellipsis. If two candidates rank equally, the call is ambiguous and fails to compile.
- Overloading is implemented through **name mangling**. `extern "C"` turns mangling off, so functions declared that way cannot be overloaded.
- You **cannot** overload on return type alone.

### Q12. What is undefined behavior (UB)? Give common examples. ⭐
**Key points:**
- UB is behavior the standard places no requirements on. The compiler is allowed to assume UB never happens and optimize on that basis, which can produce surprising results.
- Common examples:
  - Dereferencing a null or dangling pointer.
  - Out-of-bounds array access.
  - **Signed** integer overflow.
  - Reading uninitialized variables.
  - Data races.
  - Deleting the same pointer twice.
  - Modifying a string literal.
  - Deleting a derived object through a base pointer that has no virtual destructor.
- Catch UB with `-fsanitize=address,undefined`.

---

## 2. Pointers & References (Q13–Q23)

### Q13. What is the difference between a pointer and a reference? ⭐
**Key points:**
- A **reference** is an alias. It must be initialized, cannot legitimately be null, cannot be reseated to refer to something else, and has no arithmetic.
- A **pointer** is an object that stores an address. It can be null, reassigned, used in arithmetic, and point to another pointer (`T**`).
- Use references for required parameters. Use pointers when the value is optional or must change what it points to.

### Q14. Explain `const int* p`, `int const* p`, `int* const p`, and `const int* const p`. ⭐
**Key points:** Read the declaration from right to left.
```cpp
int a = 1, b = 2;
const int* p1 = &a;        // pointer to const int:  *p1 = 5 ✗   p1 = &b ✓   (same as int const*)
int* const p2 = &a;        // const pointer to int:  *p2 = 5 ✓   p2 = &b ✗
const int* const p3 = &a;  // const pointer to const int: both ✗
```

### Q15. What are null, dangling, and wild pointers? How do you avoid them? ⭐
**Key points:**
- A **null** pointer points to nothing. Dereferencing it is UB.
- A **dangling** pointer refers to memory that has been freed or has gone out of scope. Typical causes are returning the address of a local, using memory after `delete`, and keeping a pointer into a `vector` after it reallocates.
- A **wild** pointer was never initialized.
- To avoid them: always initialize pointers, use RAII and smart pointers, never return pointers or references to locals, and run AddressSanitizer.

### Q16. Why is `nullptr` preferred over `NULL` or `0`?
**Key points:**
- `nullptr` has type `std::nullptr_t`. It converts to any pointer type but **not** to an integer.
- `NULL` is usually `0` or `0L`, so `f(NULL)` can call `f(int)` instead of `f(char*)`.
- `nullptr` also behaves correctly in templates and with perfect forwarding.

### Q17. How does pointer arithmetic work? What is the relationship between arrays and pointers (array decay)? ⭐
**Key points:**
- `p + n` moves forward by `n * sizeof(*p)` bytes. Subtracting two pointers into the same array gives an element count (`ptrdiff_t`).
- `a[i]` is exactly `*(a + i)`.
- An array passed to a function **decays** to a pointer to its first element and loses its size, so `sizeof(arr)` inside the function returns the pointer size.
- Pointer arithmetic outside the array is UB, except for the one-past-the-end position.
- *Vision angle:* to walk an image row by row, compute `uint8_t* row = data + y * stride;`.

### Q18. What is a `void*`? What can and can't you do with it?
**Key points:**
- A `void*` can point to any object type.
- You cannot dereference it or do arithmetic on it; you must cast it back to the original type first.
- It is common in C APIs such as `malloc`, `memcpy` and callback "user data". In C++, prefer templates, `std::variant` or `std::any`.

### Q19. What are function pointers? How do they compare to functors, lambdas, and `std::function`?
**Key points:**
- Function pointer syntax: `int (*fp)(int, int) = &add;`. They are used for callbacks.
- **Functors** are classes with `operator()`. They can hold state and are easily inlined.
- **Lambdas** are functors that the compiler generates for you. A lambda with no captures converts to a function pointer.
- **`std::function`** is a type-erased wrapper that can hold any callable. It is flexible, but it may heap-allocate and it blocks inlining.
- *Vision angle:* in per-pixel hot loops, prefer template parameters or lambdas over `std::function`.

### Q20. What is the `this` pointer? What is its type inside a const member function?
**Key points:**
- `this` is an implicit pointer to the object a non-static member function was called on.
- Its type is `T*`, or `const T*` inside a const member function. It does not exist in static member functions.
- Typical uses are method chaining (`return *this;`), disambiguating member names, and passing the object to other code.

### Q21. Explain `std::unique_ptr`, `std::shared_ptr`, and `std::weak_ptr`. When do you use each? ⭐
**Key points:**
- **`unique_ptr`:** exclusive ownership and move-only. With the default deleter it costs nothing over a raw pointer. Custom deleters let it manage C handles, files or GPU buffers.
- **`shared_ptr`:** shared ownership through a reference count. It costs a control block plus atomic increments and decrements.
- **`weak_ptr`:** a non-owning observer of a `shared_ptr`. It breaks reference cycles, and `lock()` returns a `shared_ptr` if the object is still alive.
- Default to `unique_ptr`. Create them with `std::make_unique` and `std::make_shared`.

### Q22. How does `shared_ptr` work internally? Why prefer `make_shared`? Is `shared_ptr` thread-safe? ⭐
**Key points:**
- A `shared_ptr` holds a pointer to the object and a pointer to a **control block**. The control block stores the strong count, the weak count and the deleter.
- When the last strong reference goes away, the object is destroyed. When the last weak reference goes away, the control block is freed.
- `make_shared` makes **one allocation** for both the object and the control block, which gives better locality and exception safety. The downside is that the object's memory is not freed until the last `weak_ptr` is gone.
- **Thread safety:** updates to the reference count are atomic and safe. Access to the pointed-to object is **not** synchronized. Modifying the same `shared_ptr` instance from several threads is a data race unless you use `std::atomic<std::shared_ptr<T>>` (C++20).

### Q23. What is a circular reference with `shared_ptr`, and how do you fix it? 💻
**Key points:**
- If A holds a `shared_ptr` to B and B holds one back to A, neither count ever reaches zero, so both objects leak.
- The fix is to make the back-pointer a `weak_ptr`, for example child → parent in a tree or observer → subject.

---

## 3. Memory Management (Q24–Q31)

### Q24. What is the difference between stack and heap memory? Describe the memory layout of a C++ process. ⭐
**Key points:**
- **Stack:** automatic storage, allocated last-in first-out. Allocation is very fast and memory is freed when the scope ends, but the stack is small (typically 1–8 MB).
- **Heap (free store):** dynamic storage. Allocation is slower and can fragment memory, but it can be very large.
- **Process layout:** text (code), read-only data, data (initialized globals), BSS (zero-initialized globals), heap (grows up), memory-mapped region (shared libraries), and stack (grows down).
- *Vision angle:* one 4K RGB frame is about 25 MB, so a local array that size would overflow the stack. Image buffers belong on the heap.

### Q25. What are the differences between `new`/`delete` and `malloc`/`free`? ⭐
**Key points:**
- `new` allocates memory **and runs the constructor**. It is type-safe and throws `std::bad_alloc` on failure (unless you use `new (std::nothrow)`). It can be overloaded per class.
- `malloc` returns raw `void*` bytes, runs no constructor, and returns `NULL` on failure.
- Never mix the two families. For example, calling `delete` on memory from `malloc` is UB.

### Q26. What's the difference between `delete` and `delete[]`? What happens if you mix them up? ⭐
**Key points:**
- `delete[]` destroys every element of the array and then frees the block. Implementations often store the element count just before the array.
- Using the wrong one is UB: it can leak memory, corrupt the heap or crash.
- Better still, avoid both and use `std::vector` or `std::unique_ptr<T[]>`.

### Q27. What is RAII? Give examples. ⭐
**Key points:**
- RAII stands for *Resource Acquisition Is Initialization*: the constructor acquires a resource and the destructor releases it.
- Destructors run automatically, including during exception stack unwinding, so the cleanup is exception-safe.
- Standard library examples: `std::vector`, `std::string`, smart pointers, `std::lock_guard`, `std::fstream`, `std::jthread`.
- Examples of your own you could mention: a camera-handle wrapper, a GPU buffer, a file descriptor, a scoped timer.
- RAII is the reason modern C++ code has "no naked `new`/`delete`".

### Q28. What is a memory leak? How do you detect and prevent leaks? ⭐
**Key points:**
- A leak is memory that was allocated, never freed, and is no longer reachable.
- Common causes: a missing `delete`, an exception thrown between `new` and `delete`, `shared_ptr` cycles, and a base class without a virtual destructor.
- Detection tools: Valgrind (memcheck), AddressSanitizer/LeakSanitizer, heaptrack, the Visual Studio debug heap.
- Prevention: RAII, smart pointers and standard containers.
- *Vision angle:* a long-running service that leaks a few KB per frame at 30 fps will eventually crash.

### Q29. What is shallow copy vs deep copy? When does the default copy constructor cause problems?
**Key points:**
- The default copy is memberwise, so a raw-pointer member gets copied as just the address. Both objects then share one buffer, which leads to a double `delete` or unexpected aliasing.
- A deep copy allocates a new buffer and copies the data.
- Fix it by writing the copy operations yourself (Rule of Three/Five), or better, by holding resources in containers and smart pointers (Rule of Zero).
- *Vision angle:* assigning a `cv::Mat` is **shallow** (the buffer is reference-counted). `clone()` makes a **deep** copy.

### Q30. What is structure padding and alignment? How do you compute `sizeof` of a struct? 💻
**Key points:**
- Each member is placed at an address that is a multiple of its alignment, and the total size is rounded up to a multiple of the largest alignment.
- Ordering members from largest to smallest reduces padding:
```cpp
struct A { char a; int b; char c; };   // sizeof == 12 (1 + 3 pad + 4 + 1 + 3 pad)
struct B { int b; char a; char c; };   // sizeof == 8  (4 + 1 + 1 + 2 pad)
```
- Related tools: `alignof`, `alignas(32)` (for SIMD loads and cache lines), and `#pragma pack` (which can make access slower or unaligned).

### Q31. What is placement new and when would you use it?
**Key points:**
- `new (buffer) T(args)` constructs an object in memory that is already allocated; it does not allocate anything.
- You must call the destructor yourself (`p->~T()`) and must **not** call `delete` on the object. The buffer must be correctly aligned.
- It is used in memory pools, custom allocators, the internals of `std::vector` and `std::optional`, and in real-time code.
- *Vision angle:* preallocated frame-buffer pools avoid heap allocation on the hot path.

---

## 4. Classes & Objects (Q32–Q42)

### Q32. What are the different kinds of constructors in C++? ⭐
**Key points:**
- The kinds are: default, parameterized, copy (`T(const T&)`), move (`T(T&&) noexcept`), **delegating** (one constructor calls another of the same class), **converting** (a non-explicit constructor with one argument), and **inheriting** (`using Base::Base;`).
- The compiler generates a default constructor only if you declare no constructors at all.

### Q33. What is a member initializer list? Why is it preferred, and when is it mandatory? ⭐
**Key points:**
- It initializes members directly, instead of default-constructing them and then assigning in the body.
- It is **mandatory** for `const` members, reference members, and any member or base class that has no default constructor.
- **Members are initialized in declaration order, not in the order of the list.** This is a classic bug:
```cpp
class Buffer {
    int* data_;   // declared first → initialized first
    int  size_;
public:
    Buffer(int n) : size_(n), data_(new int[size_]) {}  // BUG: size_ is still garbage here
};
```

### Q34. What's the difference between the copy constructor and the copy assignment operator? Why must the copy constructor take its argument by reference? ⭐
**Key points:**
- The **copy constructor** creates a new object: `T b = a;`, `T b(a);`, or when passing or returning by value.
- **Copy assignment** overwrites an object that already exists (`b = a;`). It must handle self-assignment, release the old resources, and return `T&`.
- If the copy constructor took its parameter by value, passing the argument would itself need a copy, which recurses forever. The compiler rejects that declaration.

### Q35. What is the Rule of Three / Rule of Five / Rule of Zero? ⭐
**Key points:**
- **Rule of Three:** if you define a destructor, copy constructor or copy assignment operator, you probably need all three.
- **Rule of Five:** C++11 adds the move constructor and move assignment operator to that set.
- **Rule of Zero:** hold resources in RAII members (`std::vector`, `std::unique_ptr`) so the compiler-generated special members are correct and you write none of them. This is the preferred approach.
- Gotcha: declaring a destructor suppresses the implicit move operations, so the class silently falls back to copying.

### Q36. When is a destructor called? Can a destructor be overloaded, be virtual, or throw?
**Key points:**
- A destructor runs when:
  - A local object goes out of scope.
  - `delete` is called on a heap object.
  - A temporary reaches the end of its full expression.
  - The program exits (for static objects).
  - The stack unwinds after an exception.
- Members and bases are destroyed in the **reverse order** of construction.
- A destructor cannot be overloaded because it takes no parameters. It can be virtual, and even pure virtual, but a pure virtual destructor still needs a body.
- Destructors are implicitly `noexcept`. Throwing from one during stack unwinding calls `std::terminate`.

### Q37. What does the `explicit` keyword do? ⭐
**Key points:**
- It stops a constructor or conversion operator from being used for implicit conversions.
- Without it, `Image img = 5;` or `process(5)` compiles silently if `Image(int)` exists.
- Make single-argument constructors `explicit` by default. C++20 adds `explicit(bool)`.

### Q38. What are `= default` and `= delete` used for?
**Key points:**
- `= default` asks the compiler to generate a special member function. It keeps the class trivial where possible and documents intent.
- `= delete` removes a function. Common uses:
  - Making a class non-copyable: `Camera(const Camera&) = delete;` for objects that own a device handle.
  - Blocking unwanted overloads or conversions: `void setGain(double) = delete;`.

### Q39. What are const member functions and the `mutable` keyword?
**Key points:**
- A const member function promises not to change the object's observable state. Only const member functions can be called through a const object or a `const&`.
- You can overload on const, for example a const and a non-const `operator[]`.
- A `mutable` member can be changed even inside const functions. It is meant for caches, mutexes and lazily computed values (such as a cached histogram), not for logical state.

### Q40. What are friend functions and friend classes? Do they break encapsulation?
**Key points:**
- A friend function or class gets access to private and protected members.
- Typical uses: `operator<<`, symmetric binary operators, iterator/container pairs, and test fixtures.
- Friendship is **not** inherited, transitive or mutual.
- Used sparingly, friendship arguably strengthens encapsulation, because the alternative is making members public.

### Q41. What is `sizeof` an empty class? Why isn't it zero? What changes if you add a virtual function?
**Key points:**
- An empty class has size **1**, so that two distinct objects always have distinct addresses.
- The *empty base optimization* lets an empty base class take 0 bytes. C++20 `[[no_unique_address]]` does the same for members.
- Adding a virtual function adds a hidden **vptr**, so the size becomes 8 on a typical 64-bit system.

### Q42. Can a constructor be private? Can it be virtual? What happens if you call a virtual function from a constructor? ⭐
**Key points:**
- A **private** constructor is used for singletons, named factory functions, and classes that should never be instantiated.
- A constructor **cannot be virtual**: the object's type must be known to construct it, and the vptr is not set up yet. The "virtual constructor" idiom uses a virtual `clone()` or a factory instead.
- Calling a virtual function in a constructor or destructor calls **the current class's version**, because the derived part does not exist yet or has already been destroyed:
```cpp
struct Base {
    Base() { hello(); }
    virtual void hello() { std::cout << "Base\n"; }
};
struct Derived : Base {
    void hello() override { std::cout << "Derived\n"; }
};
Derived d;   // prints "Base"
```
- Calling a pure virtual function this way is UB and typically crashes with "pure virtual function called".

---

## 5. OOP: Inheritance & Polymorphism (Q43–Q54)

### Q43. What are the four pillars of OOP? Explain each with a C++ example. ⭐
**Key points:**
- **Encapsulation:** private data behind a public interface. For example, an `Image` class keeps width, height and buffer consistent.
- **Abstraction:** show *what* something does and hide *how*. For example, `IFeatureDetector::detect()`.
- **Inheritance:** reuse through an is-a relationship. For example, `UsbCamera : public Camera`.
- **Polymorphism:** one interface, many implementations, through virtual functions, overloading and templates.

### Q44. What is the difference between encapsulation and abstraction?
**Key points:**
- **Abstraction** is a design-level idea: model the essential behavior and hide the complexity, usually through interfaces and abstract classes.
- **Encapsulation** is an implementation-level mechanism: bundle data with the methods that use it and restrict access with `private`/`protected` to protect invariants.
- In short, abstraction decides what to show and encapsulation hides the rest.

### Q45. Explain public, protected, and private inheritance. ⭐
**Key points:** This table shows the access a base-class member has inside the derived class.

| Base member | `public` inheritance | `protected` inheritance | `private` inheritance |
|-------------|----------------------|-------------------------|-----------------------|
| public      | public               | protected               | private               |
| protected   | protected            | protected               | private               |
| private     | not accessible       | not accessible          | not accessible        |

- Public inheritance models an *is-a* relationship. Private inheritance means *implemented in terms of*, and composition is usually better for that.
- The default is `private` inheritance for `class` and `public` for `struct`.

### Q46. What is the difference between compile-time (static) and runtime (dynamic) polymorphism? ⭐
**Key points:**
- **Static** polymorphism means overloading, operator overloading, templates and CRTP. It is resolved at compile time (*early binding*), has no runtime cost, can be inlined, and may bloat the code.
- **Dynamic** polymorphism means virtual functions called through a base pointer or reference. It is resolved at runtime through the vtable (*late binding*), which makes it flexible: plugins, or heterogeneous containers like `std::vector<std::unique_ptr<Filter>>`.
- *Vision angle:* avoid a virtual call per pixel. Call virtually once per image or tile, or use templates.

### Q47. How do virtual functions work internally? Explain the vtable and vptr. ⭐
**Key points:**
- Each polymorphic **class** has one **vtable**: a static array of function pointers, one entry per virtual function.
- Each **object** carries a hidden **vptr** that points to its class's vtable. Constructors set it.
- A virtual call loads the vptr, indexes into the vtable, and makes an indirect call.
- The costs are an extra pointer per object, an indirect branch, and the loss of inlining.
- The standard does not require this mechanism, but every major compiler uses it. With multiple inheritance an object can have several vptrs.

### Q48. What is a pure virtual function and an abstract class? How do you define an interface in C++? ⭐
**Key points:**
- `virtual void f() = 0;` makes the class abstract, so it cannot be instantiated. A derived class becomes concrete only once it overrides every pure virtual function.
- An interface is an abstract class with only pure virtual functions plus a virtual destructor:
```cpp
class IFrameProcessor {
public:
    virtual ~IFrameProcessor() = default;
    virtual void process(Frame& frame) = 0;
};
```
- A pure virtual function *can* have a body, which you call as `Base::f()`. A pure virtual destructor *must* have one.

### Q49. Why should a base class have a virtual destructor? What happens if it doesn't? ⭐
**Key points:**
- Deleting a derived object through a base pointer is **UB** if the base destructor is not virtual. In practice, usually only the base destructor runs and the derived class's resources leak.
```cpp
struct Base { ~Base() {} };                        // not virtual!
struct Derived : Base { std::vector<int> big = std::vector<int>(1'000'000); };
Base* p = new Derived;
delete p;                                          // UB: ~Derived never runs
```
- The rule: a polymorphic base gets a **public virtual** destructor, or a **protected non-virtual** one if deleting through the base is not allowed.

### Q50. What is the diamond problem? How does virtual inheritance solve it? ⭐
**Key points:**
- If `D` derives from both `B` and `C`, and both derive from `A`, then `D` contains **two** `A` subobjects. Member access becomes ambiguous and the state is duplicated.
- Declaring `class B : virtual public A` (and the same for `C`) gives `D` a single shared `A`. The **most-derived class** constructs the virtual base.
- Virtual inheritance adds an extra indirection. Pure-interface bases or composition usually avoid the problem entirely.

### Q51. What's the difference between overloading, overriding, and hiding? ⭐
**Key points:**
- **Overloading:** same scope, different signatures, resolved at compile time.
- **Overriding:** a derived class supplies a virtual function with the same signature, resolved at runtime.
- **Hiding:** declaring a function in the derived class with the same *name* hides **every** base overload with that name:
```cpp
struct Base    { void draw(int); void draw(double); };
struct Derived : Base {
    void draw(const std::string&);   // hides both Base::draw overloads
    // using Base::draw;             // uncomment to bring them back
};
Derived d;
d.draw(42);   // compile error: Base::draw(int) is hidden
```

### Q52. What do the `override` and `final` keywords do?
**Key points:**
- `override` makes the compiler check that the function really overrides a base virtual function. That catches typos and `const` mismatches.
- `final` on a function prevents further overriding. `final` on a class prevents deriving from it. Both enable devirtualization.
- Gotcha: **default arguments of virtual functions are bound statically.** The default comes from the static type of the pointer, while the function body comes from the dynamic type.

### Q53. What is object slicing? How do you avoid it? ⭐ 💻
**Key points:**
- Copying a derived object into a base-class *value* keeps only the base part. The derived data and the overridden behavior are lost:
```cpp
struct Shape  { virtual std::string name() const { return "Shape"; } };
struct Circle : Shape { double r = 1.0; std::string name() const override { return "Circle"; } };

void byValue(Shape s)      { std::cout << s.name(); }  // "Shape"  ← sliced
void byRef(const Shape& s) { std::cout << s.name(); }  // "Circle"
```
- To avoid it, pass by reference or pointer and store `std::vector<std::unique_ptr<Shape>>`. You can also make the base abstract or non-copyable.

### Q54. In what order are constructors and destructors called with inheritance and member objects? ⭐ 💻
**Key points:**
- Construction order: virtual bases, then direct bases in declaration order, then **members in declaration order**, then the constructor body. Destruction runs in exactly the **reverse** order.
```cpp
struct Member  { Member()  { std::cout << "Member ";  } ~Member()  { std::cout << "~Member ";  } };
struct Base    { Base()    { std::cout << "Base ";    } ~Base()    { std::cout << "~Base ";    } };
struct Derived : Base {
    Member m;
    Derived()  { std::cout << "Derived ";  }
    ~Derived() { std::cout << "~Derived "; }
};
int main() { Derived d; }
// Output: Base Member Derived ~Derived ~Member ~Base
```

---

## 6. Operator Overloading & Move Semantics (Q55–Q60)

### Q55. How does operator overloading work? Which operators can't be overloaded? Member or non-member? ⭐
**Key points:**
- **Cannot** be overloaded: `::`, `.`, `.*`, `?:`, `sizeof`, `typeid`, `alignof`.
- **Must** be member functions: `=`, `[]`, `()`, `->`, and conversion operators.
- **Prefer non-members** (often friends) for symmetric binary operators, so the left operand can be converted too, and for `<<`/`>>`, where the left operand is a stream.
- Keep the semantics natural and implement `+` in terms of `+=`. C++20 adds `operator<=>` (spaceship) for comparisons.
- *Vision angle:* arithmetic on `Vec3`, `Point` or `Pixel` types, and `operator<<` for logging.

### Q56. How do you write a correct copy assignment operator? What is the copy-and-swap idiom? 💻
**Key points:**
- A correct copy assignment operator must handle self-assignment, give the strong exception guarantee (allocate the new buffer before freeing the old one), and return `*this`.
- **Copy-and-swap** does all of this, and one operator handles both copy and move assignment:
```cpp
class Buffer {
public:
    Buffer& operator=(Buffer other) noexcept {   // copy or move happens at the call site
        swap(*this, other);
        return *this;
    }                                            // old contents die with 'other'
    friend void swap(Buffer& a, Buffer& b) noexcept {
        using std::swap;
        swap(a.size_, b.size_);
        swap(a.data_, b.data_);
    }
private:
    size_t size_ = 0;
    int*   data_ = nullptr;
};
```

### Q57. What are move semantics and rvalue references? What does `std::move` actually do? ⭐ 💻
**Key points:**
- A `T&&` parameter binds to rvalues. A move constructor or move assignment **steals** the source's resources (it swaps pointers) instead of deep-copying them, which is a huge win for large buffers.
- `std::move` **moves nothing**. It is just a cast to an rvalue reference that makes the move overloads eligible.
- A moved-from object is valid but in an unspecified state.
- Don't `std::move` a local in a return statement, don't use an object after moving from it, and don't move from `const` objects (that silently copies).
- 💻 A common follow-up: implement a `String` or `Buffer` class with all five special member functions.

### Q58. Why should move constructors be marked `noexcept`?
**Key points:**
- When `std::vector` reallocates it uses `std::move_if_noexcept`. If the move constructor might throw, the vector **copies** the elements instead in order to keep the strong exception guarantee, which is much slower.
- Defaulted move operations are `noexcept` automatically when all members' move operations are.

### Q59. What are copy elision, RVO, and NRVO?
**Key points:**
- With copy elision, the compiler builds the return value directly in the caller's storage, so no copy or move happens.
- C++17 **guarantees** elision when a prvalue is returned (`return Image(w, h);`), even for types that cannot be moved. **NRVO**, for named locals, is allowed but not guaranteed.
- This is why returning large objects by value (`Image load(path)`) is cheap. `return std::move(local);` is a *pessimization* because it blocks NRVO.

### Q60. What are forwarding (universal) references and perfect forwarding?
**Key points:**
- In `template<class T> void f(T&& x)`, `T&&` with a deduced `T` is a **forwarding reference**. It binds to both lvalues and rvalues.
- **Reference collapsing:** any combination that includes `&` collapses to `&`, and only `&& &&` gives `&&`.
- `std::forward<T>(x)` preserves the original value category when passing the argument on. `emplace_back`, `make_unique` and wrapper or factory functions all use it.

---

## 7. Templates & Generic Programming (Q61–Q64)

### Q61. What are function and class templates? How are they instantiated, and why are they usually defined in headers? ⭐
**Key points:**
- The compiler generates separate code for each set of template arguments that is used. Function templates deduce their arguments, and since C++17 class templates do too (CTAD).
- The full definition must be visible where the template is instantiated, so it goes in the header. The alternative is explicit instantiation in a `.cpp` file.
- The risks are code bloat and long error messages.
- *Vision angle:* `Image<uint8_t>` vs `Image<float>`, or `template<typename T> T clamp(T v, T lo, T hi)`.

### Q62. What is template specialization (full vs partial)?
**Key points:**
- **Full specialization:** `template<> class Foo<bool> { ... };`. `std::vector<bool>` is a famous example.
- **Partial specialization** works only for class templates: `template<class T> class Foo<T*> { ... };`.
- Function templates cannot be partially specialized; use overloading instead.
- Typical uses are type traits and optimized code paths, such as a SIMD path for `uint8_t` images.

### Q63. What are variadic templates and fold expressions?
**Key points:**
- `template<typename... Args> void log(Args&&... args);` takes any number of arguments. You expand the parameter pack and get its size with `sizeof...(Args)`.
- Before C++17 packs were processed recursively. C++17 adds **fold expressions**: `(std::cout << ... << args);`.
- `std::make_unique`, `emplace_back`, `std::tuple` and printf-style loggers all use them.

### Q64. What are SFINAE, type traits, and C++20 concepts?
**Key points:**
- **SFINAE** ("substitution failure is not an error"): if substituting template arguments produces invalid code, that overload is silently dropped instead of causing an error. It is used with `std::enable_if`.
- **Type traits** such as `std::is_integral_v<T>` and `std::is_same_v<T, U>` answer questions about types at compile time. `if constexpr` branches on them at compile time.
- **Concepts** (C++20), such as `template<std::integral T>` or `requires` clauses, express the same constraints more clearly than SFINAE and give much better error messages.

---

## 8. STL Containers & Algorithms (Q65–Q72)

### Q65. What are the main components of the STL? What are the iterator categories? ⭐
**Key points:**
- **Containers:**
  - Sequence: `vector`, `deque`, `list`, `forward_list`, `array`.
  - Associative: `set`, `map`, `multiset`, `multimap`.
  - Unordered: `unordered_set`, `unordered_map`.
  - Adapters: `stack`, `queue`, `priority_queue`.
- **Iterators** connect containers to algorithms. **Algorithms** include `sort`, `find`, `transform` and `accumulate`. The other components are **function objects** and lambdas, and **allocators**.
- Iterator categories: input, output, forward, bidirectional, random-access, contiguous. `std::sort` needs random-access iterators, which is why `list` has its own `sort()` member.

### Q66. How does `std::vector` work internally? Explain size vs capacity, growth, `reserve()`, and `push_back` vs `emplace_back`. ⭐
**Key points:**
- A vector is a contiguous array described by three pointers: begin, end of the elements (size), and end of the allocation (capacity).
- When it is full, it allocates a bigger block (2× in libstdc++ and libc++, 1.5× in MSVC), moves or copies the elements over, and frees the old block. That makes `push_back` amortized **O(1)**.
- `reserve(n)` preallocates capacity, while `resize(n)` changes the size. `clear()` does not release capacity.
- `emplace_back(args...)` constructs the element in place. `push_back(obj)` copies or moves an existing object.

### Q67. What is iterator invalidation? Give the rules for `vector`, `deque`, `list`, and `map`. ⭐
**Key points:**
- **vector:** an insert invalidates **everything** if it causes a reallocation; otherwise it invalidates iterators at or after the insertion point. An erase invalidates iterators at or after the erased element.
- **deque:** inserting in the middle invalidates everything. Inserting at either end invalidates iterators but not references.
- **list/map/set:** inserts invalidate nothing. An erase invalidates only the erased element.
- **unordered_\*:** a rehash invalidates iterators but not references.
- A classic bug is erasing inside a loop. Use `it = v.erase(it);`, the erase-remove idiom, or C++20 `std::erase_if`.

### Q68. Compare `vector`, `list`, and `deque`. When would you use each? ⭐
**Key points:**

| | `vector` | `deque` | `list` |
|---|---|---|---|
| Memory | contiguous | chunked | separate nodes |
| Random access | O(1) | O(1) | O(n) |
| Insert/erase at end | amortized O(1) | O(1) at both ends | O(1) |
| Insert/erase in middle | O(n) | O(n) | O(1) given an iterator |
| Cache locality | excellent | good | poor |

- **Default to `vector`.** Thanks to cache locality it often beats `list` even when there are many middle insertions.

### Q69. Compare `std::map` and `std::unordered_map`. What does each require of its key type? ⭐
**Key points:**
- **`map`** is a red-black tree. It is ordered, operations are O(log n), and it supports `lower_bound` and range queries. The key needs `operator<` or a comparator.
- **`unordered_map`** is a hash table. Operations are O(1) on average and O(n) in the worst case, and it is unordered. The key needs `std::hash` and `operator==`.
- Gotcha: `operator[]` **inserts** a default value when the key is missing, and it cannot be used on a const map. For lookups use `find()`, `at()` or `contains()` (C++20).

### Q70. What are the container adapters `stack`, `queue`, and `priority_queue`? How is `priority_queue` implemented?
**Key points:**
- Adapters restrict the interface of an underlying container. `stack` and `queue` use `deque` by default; `priority_queue` uses `vector`.
- `priority_queue` is a binary **max-heap**: `top()` is O(1), and `push`/`pop` are O(log n). For a min-heap use `std::priority_queue<T, std::vector<T>, std::greater<T>>`.
- *Vision angle:* keeping the top-K detections by confidence score, Dijkstra's algorithm, event scheduling.

### Q71. What's the difference between C-style arrays, `std::array`, and `std::vector`?
**Key points:**
- A **C array** has a fixed size, decays to a pointer, does not know its own size, and cannot be assigned.
- **`std::array<T, N>`** has a fixed size known at compile time and is stored inline with no overhead. It can be copied, knows its `size()`, and works with STL algorithms.
- **`std::vector`** has a dynamic size and stores its elements on the heap.
- *Vision angle:* use `std::array` for small fixed things like a 3×3 kernel or an RGB pixel, and `std::vector` for image data. C++20 `std::span` is a non-owning view over any of the three.

### Q72. Which STL algorithms do you use most? What is the erase-remove idiom? ⭐ 💻
**Key points:**
- Know these algorithms:
  - `sort` (introsort, O(n log n), not stable) and `stable_sort`.
  - `find_if`, `count_if`, `transform`, `accumulate`.
  - `min_element` and `max_element`.
  - `lower_bound` and `binary_search` (on sorted ranges).
  - `nth_element` (O(n) on average, which is great for median filters).
  - `partial_sort`, `unique`.
- `std::remove_if` only moves the kept elements to the front and returns the new logical end. You still have to erase the tail:
```cpp
v.erase(std::remove_if(v.begin(), v.end(), [](int x) { return x < 0; }), v.end());
std::erase_if(v, [](int x) { return x < 0; });   // C++20 equivalent
```

---

## 9. Data Structures & Algorithms (Q73–Q82)

### Q73. What is Big-O notation? What are the complexities of common operations on core data structures? ⭐
**Key points:**

| Structure | Access | Search | Insert | Delete |
|---|---|---|---|---|
| Array / `vector` | O(1) | O(n), or O(log n) if sorted | O(n), amortized O(1) at the end | O(n) |
| Linked list | O(n) | O(n) | O(1) at a known position | O(1) at a known position |
| Hash table | — | O(1) avg, O(n) worst | O(1) avg | O(1) avg |
| Balanced BST | O(log n) | O(log n) | O(log n) | O(log n) |
| Binary heap | O(1) for the top | O(n) | O(log n) | O(log n) for the top |

- Also be ready to discuss space complexity and amortized analysis.

### Q74. Array vs linked list: what are the trade-offs? Implement a singly linked list and reverse it. ⭐ 💻
**Key points:**
- An array is contiguous and cache-friendly with O(1) indexing, but inserting in the middle and resizing are expensive.
- A linked list inserts and deletes in O(1) at a known node, but it has no random access, extra pointer overhead and poor locality.
- Reverse the list iteratively in O(n) time and O(1) space:
```cpp
Node* reverse(Node* head) {
    Node* prev = nullptr;
    while (head) {
        Node* next = head->next;
        head->next = prev;
        prev = head;
        head = next;
    }
    return prev;
}
```
- Be ready to also write insert, delete (including deleting the head) and a destructor that frees every node.

### Q75. How do you detect a cycle in a linked list? How do you find its middle element? 💻
**Key points:**
- Use **Floyd's tortoise and hare**: `slow` moves one step and `fast` moves two. If they meet, there is a cycle. This is O(n) time and O(1) space.
- To find where the cycle starts, reset one pointer to `head` and advance both one step at a time until they meet again.
- To find the middle: when `fast` reaches the end, `slow` is at the middle.

### Q76. Explain stacks and queues. Implement a queue using two stacks. Where are they used? ⭐ 💻
**Key points:**
- A **stack** is last-in first-out. It is used for call stacks, undo, expression evaluation, checking balanced parentheses, and DFS.
- A **queue** is first-in first-out. It is used for BFS, task scheduling, and producer-consumer buffers.
- **Queue from two stacks:** push onto `in`. To pop, if `out` is empty, move everything from `in` to `out`, then pop from `out`. Each operation is amortized O(1).
- Follow-up: implement a fixed-capacity **ring (circular) buffer**.
- *Vision angle:* a ring buffer is the standard way to buffer camera frames.

### Q77. How does a hash table work? How are collisions handled? ⭐
**Key points:**
- The bucket index is `hash(key) % bucket_count`.
- Collisions are handled either by **separate chaining** (a list per bucket, as in `std::unordered_map`) or by **open addressing** (linear or quadratic probing, double hashing), which is more cache-friendly.
- The **load factor** is n / buckets. Exceeding the maximum triggers a rehash, which is O(n) but amortized. A poor hash function degrades lookups to O(n).
- 💻 Follow-up: design an **LRU cache** with O(1) `get` and `put`, using a `std::list` plus an `unordered_map` from key to list iterator.

### Q78. Explain binary trees and binary search trees. What are the tree traversals? ⭐ 💻
**Key points:**
- The **BST** invariant is left < node < right. Search, insert and delete take O(h): O(log n) when balanced, but O(n) if the tree degenerates, for example from inserting sorted data.
- Self-balancing trees are AVL and **red-black** (which `std::map` uses).
- Traversals:
  - **In-order** visits a BST in sorted order.
  - **Pre-order** is used to copy or serialize a tree.
  - **Post-order** is used to free a tree.
  - **Level-order** is BFS with a queue.
- Typical coding tasks: tree height, validating a BST (pass min/max bounds down), lowest common ancestor, and iterative traversal.

### Q79. What is a heap? How would you find the top-K largest elements in a large array? 💻
**Key points:**
- A heap is a complete binary tree stored in an array. The children of index `i` are `2i+1` and `2i+2`, and its parent is `(i-1)/2`. Building a heap is O(n); push and pop are O(log n).
- Three ways to get the top K:
  - A **min-heap of size K**: O(n log K) time and O(K) space.
  - `std::nth_element`: O(n) on average.
  - `std::partial_sort`.
- *Vision angle:* keeping the top-K keypoints by response, or detections by score before NMS.

### Q80. How do you represent a graph? Explain BFS vs DFS and their applications. ⭐ 💻
**Key points:**
- An **adjacency list** (`vector<vector<int>>`) uses O(V+E) space and suits sparse graphs. An **adjacency matrix** uses O(V²) space but checks for an edge in O(1).
- **BFS** uses a queue and explores level by level. It finds shortest paths in unweighted graphs.
- **DFS** uses a stack or recursion. It is used for cycle detection, topological sort and connected components.
- Both are O(V+E). Use Dijkstra for weighted graphs.
- *Vision angle:* an image is an implicit grid graph, so **flood fill** and **connected-component labeling** of a binary mask (4- vs 8-connectivity) are BFS/DFS problems. On large images, use an explicit stack or queue instead of recursion to avoid a stack overflow.

### Q81. Compare quicksort, mergesort, and heapsort. What is a stable sort? Implement binary search. ⭐ 💻
**Key points:**
- **Quicksort:** O(n log n) on average and O(n²) in the worst case. It sorts in place, is cache-friendly, and is not stable.
- **Mergesort:** always O(n log n) and stable, but needs O(n) extra memory. It is good for linked lists and external sorting.
- **Heapsort:** O(n log n) in the worst case and in place, but not stable and poor for the cache.
- `std::sort` is usually introsort (quicksort + heapsort + insertion sort).
- A sort is **stable** if equal elements keep their relative order.
- **Binary search** is O(log n). Use `mid = lo + (hi - lo) / 2` to avoid overflow and be careful with off-by-one errors. Know `lower_bound` and `upper_bound`.

### Q82. Matrix problems: rotate an image 90°, transpose it, or apply a 3×3 convolution. How does row-major layout affect performance? ⭐ 💻
**Key points:**
- A 2D image lives in a 1D buffer: `index = y * stride + x * channels + c`.
- Loop with **y in the outer loop and x in the inner loop** so memory is read sequentially. Walking column by column causes cache misses.
- **Rotate 90° clockwise:** `dst[x][H-1-y] = src[y][x]`. For a square matrix you can do it in place: transpose, then reverse each row.
- **Convolution:**
  - Handle the borders (zero, clamp/replicate, or reflect).
  - Accumulate in a wider type (`int` or `float`) and **saturate** the result back to [0, 255].
  - **Separable kernels** cut the per-pixel cost from O(k²) to O(2k).
  - **Integral images** make box filters O(1) per pixel.

---

## 10. Modern C++ (Q83–Q86)

### Q83. What are the most important features of C++11/14/17/20? ⭐
**Key points:**
- **C++11:**
  - Language: `auto`, range-for, lambdas, move semantics, `nullptr`, `constexpr`, `override`/`final`, `enum class`, variadic templates, `static_assert`.
  - Library: smart pointers, `std::thread`, `std::mutex`, `std::atomic`, `unordered_*` containers.
- **C++14:** generic lambdas, `make_unique`, return type deduction.
- **C++17:**
  - Language: structured bindings, `if constexpr`, fold expressions, CTAD, guaranteed copy elision.
  - Library: `optional`, `variant`, `any`, `string_view`, `<filesystem>`, parallel algorithms.
- **C++20:**
  - Language: concepts, coroutines, modules, `<=>`, designated initializers.
  - Library: ranges, `std::span`, `std::jthread`, `std::format`.
- **C++23:** `std::expected`, `std::print`, and **`std::mdspan`** (a multi-dimensional view, very relevant to images).

### Q84. Explain lambda expressions: syntax, capture modes, and pitfalls. ⭐
**Key points:**
- The syntax is `[captures](params) mutable -> ret { body }`. A lambda is an unnamed functor class generated by the compiler.
- Capture modes:
  - `[=]` copies at creation and `[&]` captures by reference.
  - `[x, &y]` mixes the two per variable.
  - `[this]` captures the object pointer; `[*this]` (C++17) copies the object.
  - Init-capture: `[buf = std::move(buffer)]`.
- Variables captured by value are `const` unless the lambda is `mutable`.
- **Pitfall:** capturing locals by reference in a lambda that outlives them, such as a thread, an async task or a callback, leaves **dangling references**.

### Q85. What are `auto` and `decltype`? When should and shouldn't you use `auto`?
**Key points:**
- `auto` follows template deduction rules, so it **drops references and top-level const**. `auto x = constRef;` makes a copy; use `const auto&` when you want a reference.
- `auto x{1}` deduces `int`, but `auto x = {1}` deduces `std::initializer_list<int>`.
- `decltype(expr)` gives the declared type including references. Note that `decltype((x))` is a reference. `decltype(auto)` keeps the exact type.
- Use `auto` for iterators, lambdas and long template types. Avoid it where the exact type matters, such as `uint8_t` pixel arithmetic, where values are promoted to `int`.

### Q86. What are `std::optional`, `std::variant`, `std::string_view`, and `std::span`? When would you use them?
**Key points:**
- **`optional<T>`** holds a value or nothing, for example `std::optional<Rect> findFace(const Image&)` instead of a sentinel or null.
- **`variant<A, B>`** is a type-safe union used with `std::visit`.
- **`string_view`** is a non-owning, allocation-free view of characters. Beware dangling views of temporary strings.
- **`span<T>`** is a non-owning pointer plus size over contiguous memory. It is ideal for passing image rows or buffers without copying them and without losing their size.

---

## 11. Concurrency & Multithreading (Q87–Q92)

### Q87. How do you create threads in C++? What's the difference between `join()` and `detach()`? ⭐
**Key points:**
- `std::thread t(func, args...);` **copies** its arguments; wrap them in `std::ref` to pass a reference.
- `join()` waits for the thread to finish. `detach()` lets it run on its own, which is risky because it may outlive the data it uses.
- Destroying a thread that is still **joinable** calls `std::terminate`. C++20 `std::jthread` joins automatically in its destructor and supports `stop_token`.

### Q88. What is a data race vs a race condition? Explain `mutex`, `lock_guard`, `unique_lock`, and `scoped_lock`. ⭐
**Key points:**
- A **data race** happens when two threads access the same memory without synchronization and at least one of them writes. It is UB.
- A **race condition** is a logic bug where the outcome depends on timing. It can exist even without a data race.
- The locking tools:
  - **`lock_guard`:** the simplest RAII lock.
  - **`unique_lock`:** movable, supports deferred and timed locking and early `unlock()`, and is required by `condition_variable`.
  - **`scoped_lock`** (C++17): locks several mutexes at once without risking deadlock.
- `shared_mutex` with `shared_lock` implements a readers-writer lock, for example many threads reading calibration data. Keep critical sections short.

### Q89. What is a deadlock? What causes it, and how do you prevent it? ⭐
**Key points:**
- A deadlock needs all four **Coffman conditions**: mutual exclusion, hold and wait, no preemption, and circular wait.
- Prevention:
  - Acquire locks in a consistent global order.
  - Use `std::scoped_lock` or `std::lock` when you need several mutexes.
  - Never call unknown code, such as callbacks, while holding a lock.
  - Use `try_lock` or timeouts.
  - Keep the locked scope minimal.
- Related problems: livelock and starvation. Locking a `std::mutex` twice from the same thread is UB.

### Q90. What is a condition variable? Implement a thread-safe producer–consumer queue. ⭐ 💻
**Key points:**
- `wait(lock, predicate)` handles **spurious wakeups** and lost wakeups. Notify with `notify_one` or `notify_all`.
- *Vision angle:* the camera thread produces frames and the worker threads consume them. A **bounded** queue provides backpressure.
```cpp
template <typename T>
class BlockingQueue {
public:
    explicit BlockingQueue(size_t capacity) : capacity_(capacity) {}

    void push(T item) {
        std::unique_lock lock(mtx_);
        notFull_.wait(lock, [&] { return queue_.size() < capacity_; });
        queue_.push(std::move(item));
        notEmpty_.notify_one();
    }

    T pop() {
        std::unique_lock lock(mtx_);
        notEmpty_.wait(lock, [&] { return !queue_.empty(); });
        T item = std::move(queue_.front());
        queue_.pop();
        notFull_.notify_one();
        return item;
    }

private:
    std::queue<T> queue_;
    std::mutex mtx_;
    std::condition_variable notEmpty_, notFull_;
    size_t capacity_;
};
```
- Follow-ups: add a `close()` method for shutdown, `try_pop` with a timeout, and a policy that drops the oldest frame when the queue is full.

### Q91. What is `std::atomic`? When would you use it instead of a mutex? What does `volatile` do (and not do)? ⭐
**Key points:**
- `std::atomic<T>` makes operations such as `load`, `store`, `fetch_add` and `compare_exchange` indivisible. For small types it is usually lock-free.
- Use atomics for counters, stop flags and statistics such as frames processed. Use a mutex when several variables must change together or for complex structures.
- Memory orders: `seq_cst` is the default, `acquire`/`release` publish data from one thread to another, and `relaxed` is enough for plain counters.
- **`volatile` is NOT for thread synchronization in C++.** It only stops the compiler from optimizing away accesses, which matters for memory-mapped hardware registers. It provides no atomicity and no ordering.

### Q92. What are `std::async`, `std::future`, and `std::promise`? What is a thread pool and why use one?
**Key points:**
- `std::async(std::launch::async, f)` runs `f` and returns a `future`. `get()` blocks until the result is ready and rethrows any exception. Gotcha: the future returned by `async` **blocks in its destructor**.
- A `promise`/`future` pair passes a value from one thread to another.
- A **thread pool** is a fixed set of workers pulling from a task queue. It avoids creating a thread per task and limits concurrency, for example when processing image tiles or ROIs in parallel.
- Also know **false sharing**: per-thread data that sits on the same cache line slows every thread down. Pad it with `alignas(64)`.

---

## 12. Exception Handling (Q93–Q94)

### Q93. How does exception handling work in C++? What is stack unwinding? What are the exception-safety guarantees? ⭐
**Key points:**
- A `throw` searches up the call stack for a matching `catch`. As the stack unwinds, the destructors of all fully constructed local objects run, which is why RAII works.
- Catch by `const&` to avoid slicing. Use `throw;` to rethrow the current exception. A second exception thrown during unwinding calls `std::terminate`.
- If a **constructor throws**, that object's destructor does **not** run, but its already-constructed members and bases are destroyed.
- The guarantees, from strongest to weakest:
  - **No-throw:** the operation never throws.
  - **Strong:** commit or roll back completely (copy-and-swap gives you this).
  - **Basic:** no leaks and invariants stay valid.
  - **None.**

### Q94. What does `noexcept` mean? Exceptions or error codes in performance-critical / real-time code?
**Key points:**
- `noexcept` promises that no exception escapes; if one does, `std::terminate` is called. It enables optimizations such as `vector` moving instead of copying. Destructors, move operations and `swap` should be `noexcept`.
- Exceptions cost nothing on the normal path but are expensive and unpredictable in timing when thrown. Some real-time and embedded codebases disable them with `-fno-exceptions`.
- Alternatives are error codes, `std::optional`, `std::expected` (C++23) and status objects.
- Use exceptions for truly exceptional situations, such as failing to open the camera. Finding no detection in a frame is normal flow, not an exception.

---

## 13. Design Principles & Patterns (Q95–Q97)

### Q95. What are the SOLID principles? When do you favor composition over inheritance? ⭐
**Key points:**
- **S**ingle responsibility.
- **O**pen/closed: extend through new classes or strategies without editing existing code.
- **L**iskov substitution: a derived object must work anywhere its base is expected. Square/Rectangle is the classic violation.
- **I**nterface segregation: prefer small, focused interfaces.
- **D**ependency inversion: depend on abstractions. For example, the pipeline depends on an `ICamera` interface rather than a vendor SDK, which also makes it mockable in tests.
- **Composition** (has-a) gives looser coupling, lets you swap behavior at runtime, and avoids fragile base classes. Use inheritance only for a genuine is-a relationship that needs polymorphic substitution.

### Q96. How do you implement a thread-safe Singleton in C++? What are its drawbacks? ⭐ 💻
**Key points:**
- The Meyers singleton is thread-safe because C++11 guarantees thread-safe initialization of static locals:
```cpp
class Config {
public:
    static Config& instance() {
        static Config inst;            // initialized once, thread-safe since C++11
        return inst;
    }
    Config(const Config&) = delete;
    Config& operator=(const Config&) = delete;
private:
    Config() = default;
};
```
- Drawbacks: it is hidden global state, makes unit testing and mocking hard, and can cause static destruction-order problems. Dependency injection is often the better choice.

### Q97. Explain the Factory, Strategy, and Observer patterns and the Pimpl idiom.
**Key points:**
- **Factory:** create an object from a config or name without exposing its concrete class. For example, `createDetector("orb")` returns `std::unique_ptr<IDetector>`.
- **Strategy:** interchangeable algorithms behind one interface, such as different thresholding or denoising algorithms. You can implement it with virtual functions, `std::function` or templates.
- **Observer:** subscribers are notified of events, such as a new frame or a detection result. Manage lifetimes with `weak_ptr` or explicit unsubscribe.
- **Pimpl:** a pointer to a private implementation hides internal details and third-party headers (vendor SDK, OpenCV) from the public header. That gives ABI stability and faster builds, at the cost of a heap allocation and an extra indirection.

---

## 14. Vision Systems, Performance & Tooling (Q98–Q100)

### Q98. How would you design an `Image` class in C++? How should it manage memory, copying, and pixel access? (How does `cv::Mat` do it?) ⭐ 💻
**Key points:**
- **Data:** width, height, channels and pixel type (a template `Image<T>` or a runtime type tag), the **stride** (bytes per row, which may include alignment padding), and a buffer (`std::vector<T>`, or an aligned `unique_ptr<T[]>`).
- **Access:** row-major `data[y * stride + x * channels + c]`. Provide a bounds-checked `at(x, y)` for safety and a raw `row(y)` pointer for hot loops. Make the accessors const-correct.
- **Ownership:** choose between **value semantics** (deep copy, Rule of Zero with `vector`) and a **shared reference-counted buffer**, which is what `cv::Mat` does. With `cv::Mat`, copying or assigning is shallow, `clone()` and `copyTo()` are deep, and an ROI is a view that shares the parent's memory.
- Make moves cheap, and align the buffer to 32 or 64 bytes for SIMD.
- **Pixel arithmetic:** `uint8_t` values promote to `int`, so clamp results back to [0, 255] (the equivalent of OpenCV's `saturate_cast`).

### Q99. How would you design a real-time, multi-threaded camera processing pipeline? What if processing is slower than the camera's frame rate? ⭐
**Key points:**
- **Stages:** a capture thread feeds a bounded queue, processing workers (detection, tracking) read from it, and they feed an output or publisher stage.
- **Memory:** use a preallocated **frame-buffer pool or ring buffer** so no allocation happens per frame. Hand frames off without copying, via move, `unique_ptr`, or `shared_ptr<const Frame>`.
- **Backpressure:** for live processing, **drop the oldest frame and keep the latest**. For offline processing, block the producer.
- **Observability:** timestamp every frame and measure FPS and latency per stage.
- **Shutdown:** use a stop flag or `stop_token`, notify all waiting threads, then join.
- **Performance:** avoid locks on the hot path (for example, a lock-free single-producer single-consumer queue) and offload to the GPU where it helps.
- **Robustness:** handle the camera disconnecting and reconnect automatically.

### Q100. How do you debug crashes and memory errors, and how do you profile and optimize C++ code (especially image processing)? ⭐
**Key points:**
- **Debugging:**
  - Debuggers: `gdb`/`lldb` (`bt`, `frame`, `print`, watchpoints) and core dumps.
  - Sanitizers: `-fsanitize=address`, `undefined` and `thread`; also Valgrind.
  - Build with `-g -Wall -Wextra`.
- **Profiling:** `perf`, VTune, Instruments (macOS) and Tracy. Use Google Benchmark for micro-benchmarks. **Measure before you optimize.**
- **Optimization priorities for image processing:**
  1. A better algorithm.
  2. Cache-friendly access (row-major traversal, struct-of-arrays vs array-of-structs).
  3. No allocations inside hot loops; reuse buffers.
  4. No per-pixel virtual calls or `std::function`.
  5. SIMD (SSE/AVX/NEON) and auto-vectorization.
  6. Multithreading (thread pool, OpenMP, TBB, `std::execution::par`).
  7. Compiler flags: `-O2`/`-O3`, `-march=native`, LTO.
  8. Lookup tables and fixed-point arithmetic.
  9. GPU offload (CUDA/OpenCL).

---

## Last-Minute Preparation Tips

- **Practice "predict the output" questions.** Q33 (initialization order), Q42 (virtual call in a constructor), Q49 (non-virtual destructor), Q51 (name hiding), Q53 (slicing) and Q54 (construction order) are written-test favourites.
- **Be able to write these by hand without an IDE:**
  - Reverse a linked list, detect a cycle, and build a queue from two stacks.
  - Binary search and BFS flood fill.
  - Matrix rotation and a 3×3 convolution.
  - A Rule-of-Five `Buffer` class, a thread-safe queue, and a Meyers singleton.
- **Always state the time and space complexity**, and mention edge cases: empty input, `nullptr`, one element, integer overflow, image borders.
- **Tie answers back to vision workloads:** large buffers, zero-copy hand-off, cache locality, real-time latency and frame dropping. This shows you understand the team's domain, not just the language.
