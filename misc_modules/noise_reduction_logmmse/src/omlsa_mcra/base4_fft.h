#pragma once
#include "head.h"
#include "Cordic.h"
#include <string.h>

#ifndef _WIN32
#define __int64 int64_t
#endif

typedef struct
{
	int real;   // 8192
	int imag;
} Complex_num;


class MY_B4_FFT {
public:
	MY_B4_FFT();
	~MY_B4_FFT();
	short initial(int N);
	short base4_fft(Complex_num *x, int sign);
	/*static MY_B4_FFT* getInstance() {
		return NSingleton;
	}*/
	void fftfix(Complex_num *x, int sign);
private:
	Cordic coc;
	int* m_Buffer_cos = nullptr, *m_Buffer_sin = nullptr;
	int* m_sort4_count = nullptr;
	int *m_e1_position = nullptr, *m_o1_position = nullptr;
	int *m_e2_position = nullptr, *m_o2_position = nullptr;
	int* m_yr = nullptr, *m_yi = nullptr;
	int* m_sort_temp_r = nullptr;
	int* m_sort_temp_i = nullptr;
	int m_fly_tempr, m_fly_tempi;
	int m_fwlen, m_finc, m_M4;
	int m_value_bit;
	int m_ifft_move_bit ;
	int m_value_limit;
	void Base4_Sort();

	//static MY_B4_FFT* NSingleton;
};
