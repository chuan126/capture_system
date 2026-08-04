#include "attitude_matrix.h"

#include <math.h>

void m2qua(double Cnb[9], double qnb[4])
{
  double C11, C12, C13, C21, C22, C23, C31, C32, C33;
  C11 = Cnb[0]; C12 = Cnb[1]; C13 = Cnb[2];
  C21 = Cnb[3]; C22 = Cnb[4]; C23 = Cnb[5];
  C31 = Cnb[6]; C32 = Cnb[7]; C33 = Cnb[8];
  double q0, q1, q2, q3, qq4;
  if (C11 >= C22 + C33)
  {
    q1 = 0.5 * sqrt(1 + C11 - C22 - C33); qq4 = 4 * q1;
    q0 = (C32 - C23) / qq4; q2 = (C12 + C21) / qq4; q3 = (C13 + C31) / qq4;
  }
  else if (C22 >= C11 + C33)
  {
    q2 = 0.5 * sqrt(1 - C11 + C22 - C33); qq4 = 4 * q2;
    q0 = (C13 - C31) / qq4; q1 = (C12 + C21) / qq4; q3 = (C23 + C32) / qq4;
  }
  else if (C33 >= C11 + C22)
  {
    q3 = 0.5 * sqrt(1 - C11 - C22 + C33); qq4 = 4 * q3;
    q0 = (C21 - C12) / qq4; q1 = (C13 + C31) / qq4; q2 = (C23 + C32) / qq4;
  }
  else
  {
    q0 = 0.5 * sqrt(1 + C11 + C22 + C33); qq4 = 4 * q0;
    q1 = (C32 - C23) / qq4; q2 = (C13 - C31) / qq4; q3 = (C21 - C12) / qq4;
  }
  qnb[0] = q0;
  qnb[1] = q1;
  qnb[2] = q2;
  qnb[3] = q3;
}

void m2att(double Cnb[9], double att[3])
{
  att[0] = asin(Cnb[7]);
  att[1] = atan2(-Cnb[6], Cnb[8]);
  att[2] = atan2(-Cnb[1], Cnb[4]);
}

void a2mat(double att[3], double Cnb[9])
{
  double si, sj, sk, ci, cj, ck;
  si = sin(att[0]); sj = sin(att[1]); sk = sin(att[2]);
  ci = cos(att[0]); cj = cos(att[1]); ck = cos(att[2]);
  Cnb[0] = cj * ck - si * sj * sk;
  Cnb[1] = -ci * sk;
  Cnb[2] = sj * ck + si * cj * sk;
  Cnb[3] = cj * sk + si * sj * ck;
  Cnb[4] = ci * ck;
  Cnb[5] = sj * sk - si * cj * ck;
  Cnb[6] = -ci * sj;
  Cnb[7] = si;
  Cnb[8] = ci * cj;
}

void a2qua(double att[3], double qnb[4])
{
  double att2[3];
  MatMul(att, 0.5, att2, 3, 1);
  double s[3], c[3];
  for (int i = 0; i < 3; i++)
  {
    s[i] = sin(att2[i]);
    c[i] = cos(att2[i]);
  }
  double sp, sr, sy, cp, cr, cy;

  sp = s[0]; sr = s[1]; sy = s[2];
  cp = c[0]; cr = c[1]; cy = c[2];
  qnb[0] = cp * cr * cy - sp * sr * sy;
  qnb[1] = sp * cr * cy - cp * sr * sy;
  qnb[2] = cp * sr * cy + sp * cr * sy;
  qnb[3] = cp * cr * sy + sp * sr * cy;
}

void q2att(double qnb[4], double att[3])
{
  double q11 = 0, q12 = 0, q13 = 0, q14 = 0;
  double q22 = 0, q23 = 0, q24 = 0;
  double q33 = 0, q34 = 0;
  double q44 = 0;
  q11 = qnb[0] * qnb[0]; q12 = qnb[0] * qnb[1];
  q13 = qnb[0] * qnb[2]; q14 = qnb[0] * qnb[3];
  q22 = qnb[1] * qnb[1]; q23 = qnb[1] * qnb[2]; q24 = qnb[1] * qnb[3];
  q33 = qnb[2] * qnb[2]; q34 = qnb[2] * qnb[3];
  q44 = qnb[3] * qnb[3];
  double C12 = 2 * (q23 - q14);
  double C22 = q11 - q22 + q33 - q44;
  double C31 = 2 * (q24 - q13);
  double C32 = 2 * (q34 + q12);
  double C33 = q11 - q22 - q33 + q44;
  att[0] = asin(C32);
  att[1] = atan2(-C31, C33);
  att[2] = atan2(-C12, C22);
}

void q2mat(double qnb[4], double Cnb[9])
{
  double q11, q12, q13, q14, q22, q23, q24, q33, q34, q44;
  q11 = qnb[0] * qnb[0]; q12 = qnb[0] * qnb[1];
  q13 = qnb[0] * qnb[2]; q14 = qnb[0] * qnb[3];
  q22 = qnb[1] * qnb[1]; q23 = qnb[1] * qnb[2]; q24 = qnb[1] * qnb[3];
  q33 = qnb[2] * qnb[2]; q34 = qnb[2] * qnb[3];
  q44 = qnb[3] * qnb[3];

  Cnb[0] = q11 + q22 - q33 - q44;
  Cnb[1] = 2 * (q23 - q14);
  Cnb[2] = 2 * (q24 + q13);
  Cnb[3] = 2 * (q23 + q14);
  Cnb[4] = q11 - q22 + q33 - q44;
  Cnb[5] = 2 * (q34 - q12);
  Cnb[6] = 2 * (q24 - q13);
  Cnb[7] = 2 * (q34 + q12);
  Cnb[8] = q11 - q22 - q33 + q44;
}

void attsyn(double Cnb[9], double att[3], double qnb[4], short int flag)
{
  if (flag == 1)
  {
    m2qua(Cnb, qnb);
    m2att(Cnb, att);
  }
  if (flag == 2)
  {
    a2mat(att, Cnb);
    m2qua(Cnb, qnb);
  }
  if (flag == 3)
  {
    double q11, q12, q13, q14, q22, q23, q24, q33, q34, q44;
    q11 = qnb[0] * qnb[0]; q12 = qnb[0] * qnb[1];
    q13 = qnb[0] * qnb[2]; q14 = qnb[0] * qnb[3];
    q22 = qnb[1] * qnb[1]; q23 = qnb[1] * qnb[2]; q24 = qnb[1] * qnb[3];
    q33 = qnb[2] * qnb[2]; q34 = qnb[2] * qnb[3];
    q44 = qnb[3] * qnb[3];

    Cnb[0] = q11 + q22 - q33 - q44;
    Cnb[1] = 2 * (q23 - q14);
    Cnb[2] = 2 * (q24 + q13);
    Cnb[3] = 2 * (q23 + q14);
    Cnb[4] = q11 - q22 + q33 - q44;
    Cnb[5] = 2 * (q34 - q12);
    Cnb[6] = 2 * (q24 - q13);
    Cnb[7] = 2 * (q34 + q12);
    Cnb[8] = q11 - q22 - q33 + q44;

    att[0] = asin(Cnb[7]);
    att[1] = atan2(-Cnb[6], Cnb[8]);
    att[2] = atan2(-Cnb[1], Cnb[4]);
  }
}

void MatAdd(
  double matrix_A[], double matrix_B[], double matrix_C[], unsigned char row, unsigned col)
{
  for (unsigned int i = 0; i < row * col; i++)
  {
    matrix_C[i] = matrix_A[i] + matrix_B[i];
  }
}

void MatSub(
  double matrix_A[], double matrix_B[], double matrix_C[], unsigned char row, unsigned col)
{
  for (unsigned int i = 0; i < row * col; i++)
  {
    matrix_C[i] = matrix_A[i] - matrix_B[i];
  }
}

void MatMul(
  double matrix_A[], double matrix_B[], double matrix_C[], unsigned char m, unsigned n,
  unsigned char p)
{
  double lSum;
  for (int i = 0; i < m; i++)
  {
    for (int j = 0; j < p; j++)
    {
      lSum = 0.0;
      for (int k = 0; k < static_cast<int>(n); k++)
      {
        lSum += matrix_A[i * n + k] * matrix_B[k * p + j];
      }
      matrix_C[i * p + j] = lSum;
    }
  }
}

void MatMul(
  double matrix_A[], double scalar, double matrix_C[], unsigned char row, unsigned col)
{
  for (unsigned int i = 0; i < row * col; ++i)
  {
    matrix_C[i] = matrix_A[i] * scalar;
  }
}
