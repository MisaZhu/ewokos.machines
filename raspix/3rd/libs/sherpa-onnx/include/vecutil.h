/**
 * vecutil.h - helpers for the EwokOS STL, whose std::vector has no data()
 * (storage is contiguous, so &v[0] works when non-empty). The EwokOS STL
 * vector/map now provide O(1) swap(), which vswap and the Tensor/Attr/Node
 * move operations are built on.
 */
#ifndef SHERPA_ONNX_PORT_VECUTIL_H_
#define SHERPA_ONNX_PORT_VECUTIL_H_

#include <vector>

template <typename T>
inline T *vdata(std::vector<T> &v) {
  return v.empty() ? (T *)nullptr : &v[0];
}

template <typename T>
inline const T *vdata(const std::vector<T> &v) {
  return v.empty() ? (const T *)nullptr : &v[0];
}

/* O(1) content exchange (kept as a function so call sites read the same
   on host and target; ewokstl swap is a plain pointer swap) */
template <typename T>
inline void vswap(std::vector<T> &a, std::vector<T> &b) {
  a.swap(b);
}

#endif  // SHERPA_ONNX_PORT_VECUTIL_H_
