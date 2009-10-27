// RUN: clang-cc -fsyntax-only -verify %s

namespace N {
  template<class T> class Y { // expected-note{{explicit instantiation refers here}}
    void mf() { } 
  };
}

// FIXME: poor diagnostic
template class Z<int>; // expected-error{{unqualified-id}}

// FIXME: This example from the standard is wrong; note posted to CWG reflector
// on 10/27/2009
using N::Y; 
template class Y<int>; // expected-error{{must occur in}}

template class N::Y<char*>; 
template void N::Y<double>::mf();
