/**
 * vecutil.h - helpers for the EwokOS STL, whose std::vector has no data()
 * or swap() (storage is contiguous, so &v[0] works when non-empty).
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

template <typename T>
inline void vswap(std::vector<T> &a, std::vector<T> &b) {
  std::vector<T> tmp = a;
  a = b;
  b = tmp;
}

#endif  // SHERPA_ONNX_PORT_VECUTIL_H_
