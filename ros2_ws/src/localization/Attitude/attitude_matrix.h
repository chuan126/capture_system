#ifndef ATTITUDE_MATRIX_H
#define ATTITUDE_MATRIX_H

// 四元数排列为[w, x, y, z]，矩阵使用行主序，欧拉角单位为rad。
void m2qua(double Cnb[9], double qnb[4]);
void m2att(double Cnb[9], double att[3]);
void a2mat(double att[3], double Cnb[9]);
void a2qua(double att[3], double qnb[4]);
void q2att(double qnb[4], double att[3]);
void q2mat(double qnb[4], double Cnb[9]);
void attsyn(double Cnb[9], double att[3], double qnb[4], short int flag);

void MatAdd(
  double matrix_A[], double matrix_B[], double matrix_C[], unsigned char row, unsigned col);
void MatSub(
  double matrix_A[], double matrix_B[], double matrix_C[], unsigned char row, unsigned col);
void MatMul(
  double matrix_A[], double matrix_B[], double matrix_C[], unsigned char m, unsigned n,
  unsigned char p);

// 原a2qua使用矩阵乘标量，所提供片段未包含该重载的实现和声明。
void MatMul(
  double matrix_A[], double scalar, double matrix_C[], unsigned char row, unsigned col);

#endif  // ATTITUDE_MATRIX_H
