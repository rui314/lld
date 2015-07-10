//===- Symbols.h ----------------------------------------------------------===//
//
//                             The LLVM Linker
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef LLD_COFF_STRING_MAP_H
#define LLD_COFF_STRING_MAP_H

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Allocator.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"
#include <algorithm>
#include <cstring>
#include <random>
#include <utility>

namespace lld {
namespace coff {

using llvm::StringRef;

template <typename T>
class StringMap {
  class Node;
  class NodePtr;

public:
  class iterator;
  StringMap();

  void dump();

  T operator[](StringRef Key);
  iterator find(StringRef Key);
  iterator findPrefix(StringRef Prefix);
  iterator begin() { return iterator(Head->getNext(0)); }
  iterator end() { return iterator(nullptr); }

  template <typename... Args>
  std::pair<iterator, bool> emplace(StringRef Key, Args&&...);

private:
  Node *forward(Node *&Curr, int Lv, size_t &Match, size_t &RMatch, StringRef Key);
  int compare(StringRef LHS, StringRef RHS, size_t &Match);
  Node *newNode(StringRef Key, int Height);
  int randomHeight();

  Node *Head;
  std::minstd_rand RNG;
  llvm::BumpPtrAllocator Alloc;
  enum { MaxHeight = 12 };
};

template <typename T> class StringMap<T>::Node {
public:
  Node *getNext(int Lv) { return Next[Lv].Ptr; }
  size_t getMatch(int Lv) { return Next[Lv].Match; }

  void setNext(int Lv, Node *N, size_t M) {
    Next[Lv].Ptr = N;
    Next[Lv].Match = M;
  }

  void dump() {
    llvm::dbgs() << Key;
    for (int I = 0; I < Height; ++I) {
      Node *Next = getNext(I);
      llvm::dbgs() << " " << (Next ? Next->Key : "(null)") << "(" << getMatch(I) << ") ";
    }
    llvm::dbgs() << "\n";
  }

  StringRef Key;
  T Value;
  int Height;
  NodePtr Next[1];
};

template <typename T> class StringMap<T>::NodePtr {
public:
  Node *Ptr;
  size_t Match;
};

template <typename T>
class StringMap<T>::iterator
    : public std::iterator<std::forward_iterator_tag, std::pair<StringRef, T>> {
public:
  iterator(Node *N) : Curr(N) {}
  iterator(const iterator &It) : Curr(It.Curr) {}
  bool operator==(const iterator &RHS) { return Curr == RHS.Curr; }
  bool operator!=(const iterator &RHS) { return Curr != RHS.Curr; }

  iterator &operator++() {
    Curr = Curr->getNext(0);
    return *this;
  }

  std::pair<StringRef, T *> operator*() {
    return std::make_pair(Curr->Key, &Curr->Value);
  }

  iterator operator++(int) {
    iterator Tmp(*this);
    operator++();
    return Tmp;
  }

private:
  Node *Curr;
};

template <typename T>
StringMap<T>::StringMap()
    : Head(newNode("", MaxHeight)), RNG(std::random_device()()) {}

template <typename T>
void StringMap<T>::dump() {
  for (Node *N = Head->getNext(0); N; N = N->getNext(0))
    N->dump();
}

template <typename T> T StringMap<T>::operator[](StringRef Key) {
  auto Pair = insert(std::make_pair(Key, T()));
  iterator It = Pair.first;
  return *It;
}

template <typename T>
typename StringMap<T>::iterator StringMap<T>::find(StringRef Key) {
  Node *Curr = Head;
  size_t Match = 0;
  for (int Lv = MaxHeight - 1; Lv >= 0; --Lv) {
    size_t RMatch;
    if (Node *Found = forward(Curr, Lv, Match, RMatch, Key))
      return iterator(Found);
  }
  return iterator(nullptr);
}

template <typename T>
typename StringMap<T>::iterator StringMap<T>::findPrefix(StringRef Prefix) {
  Node *Curr = Head;
  size_t Match = 0;
  for (int Lv = MaxHeight - 1; Lv >= 0; --Lv) {
    size_t RMatch;
    if (Node *Found = forward(Curr, Lv, Match, RMatch, Prefix))
      return iterator(Found);
  }
  Node *Next = Curr->getNext(0);
  if (Next && Next->Key.startswith(Prefix))
    return iterator(Next);
  return iterator(nullptr);
}

template <typename T>
template <typename... ArgsT>
std::pair<typename StringMap<T>::iterator, bool>
StringMap<T>::emplace(StringRef Key, ArgsT&&... Args) {
  Node *Prev[MaxHeight];
  size_t LMatch[MaxHeight];
  size_t RMatch[MaxHeight];

  Node *Curr = Head;
  size_t Match = 0;
  for (int Lv = MaxHeight - 1; Lv >= 0; --Lv) {
    if (Node *Found = forward(Curr, Lv, Match, RMatch[Lv], Key))
      return std::make_pair(iterator(Found), false);
    Prev[Lv] = Curr;
    LMatch[Lv] = Match;
  }

  int Height = randomHeight();
  Node *New = newNode(Key, Height);
  new (&New->Value) T(std::forward<ArgsT>(Args)...);
  for (int Lv = 0; Lv < Height; ++Lv) {
    New->setNext(Lv, Prev[Lv]->getNext(Lv), RMatch[Lv]);
    Prev[Lv]->setNext(Lv, New, LMatch[Lv]);
  }
  return std::make_pair(iterator(New), true);
}

template <typename T>
inline typename StringMap<T>::Node *
StringMap<T>::forward(Node *&Curr, int Lv, size_t &Match, size_t &RMatch,
                             StringRef Key) {
  Node *Next = Curr->getNext(Lv);
  for (; Next; Curr = Next, Next = Next->getNext(Lv)) {
    RMatch = Curr->getMatch(Lv);
    if (Match > RMatch)
      return nullptr;
    if (Match < RMatch)
      continue;
    int Comp = compare(Key, Next->Key, RMatch);
    if (Comp == 0)
      return Next;
    if (Comp < 0)
      return nullptr;
    Match = RMatch;
  }
  RMatch = 0;
  return nullptr;
}

template <typename T>
inline int StringMap<T>::compare(StringRef A, StringRef B, size_t &Match) {
  size_t E = std::min(A.size(), B.size());
  for (size_t I = Match; I < E; ++I) {
    int Comp = A[I] - B[I];
    if (Comp == 0)
      continue;
    Match = I;
    return Comp;
  }
  Match = E;
  return A.size() - B.size();
}

template <typename T>
inline typename StringMap<T>::Node *StringMap<T>::newNode(StringRef Key, int Height) {
  size_t Size = sizeof(Node) + sizeof(NodePtr) * (Height - 1);
  void *Buf = Alloc.Allocate(Size, llvm::alignOf<Node>());
  memset(Buf, 0, Size);
  Node *N = reinterpret_cast<Node *>(Buf);
  N->Key = Key;
  N->Height = Height;
  return N;
}

template <typename T>
inline int StringMap<T>::randomHeight() {
  std::uniform_int_distribution<> Rand(0, 3);
  for (int I = 1; I < MaxHeight; ++I)
    if (Rand(RNG))
      return I;
  return MaxHeight;
}

} // namespace coff
} // namespace lld

#endif
