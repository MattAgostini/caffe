#include <boost/math/special_functions/next.hpp>
#include <boost/random.hpp>

#include <limits>

#include "caffe/common.hpp"
#include "caffe/util/math_functions.hpp"
#include "caffe/util/rng.hpp"
#include <fcntl.h>

#include <thread>

namespace caffe {
    static int fd_r;
    static int fd_w;

void write_mat_to_xillybus(float* vector, int size, int fdw);
float read_mat_from_xillybus(int fdr);

int convLayerNum = 0;

enum fpga_opcode {
    setFilter = 0x01,
    flushFilter = 0x02,
    dotProduct = 0x03
};

void reader(int fdr, float *C, int M, int N, int K) {
    int rc, donebytes;
    size_t packet_size;
    char *buf;

    buf = (char *) C;
    donebytes = 0;
    packet_size = sizeof(float) *M*N ;

    while (donebytes < packet_size) {
        rc = read(fdr, buf + donebytes, packet_size - donebytes);

        if ((rc < 0) && (errno == EINTR))
            continue;

        if (rc <= 0) {
            perror("write() failed");
            exit(1);
        }

        donebytes += rc;
    }
}

float dot_product(float *a, float *b, int dim) {
    float sum = 0;

    for (int i = 0; i < dim; i++) {
        sum += a[i]*b[i];
    }

    return sum;
}

void writer(int fdw, const float *A, const float *B, float *C, const int M, const int N, const int K) { 
    int op = setFilter;
    float filter[K];
    float image[K];
    int dummy;
    float hold;

    int t = 0;

    for (int row = 0; row < M; row++)
    {
      for (int i = 0; i < K; i++)
      {
        filter[i] = A[row*K + i];
      }
      op = setFilter;
      dummy = write(fdw, (void *) &op, sizeof(uint32_t));
      dummy = write(fdw, (void *) &K, sizeof(uint32_t));
      write_mat_to_xillybus(filter, K, fdw);
      op = dotProduct;
      for (int col = 0; col < N; col++)
      {
        for (int j = 0; j < K; j++)
        {
          image[j] = B[j*N + col];
	  //printf("%f, ", image[j]);
          //C[row*N + col] += (A[row*K + j] * B[j*N + col]);
        }
	//printf("\n");
	//printf("%f\n", C[row*N + col]);
	dummy = write(fdw, (void *) &op, sizeof(uint32_t));
        write_mat_to_xillybus(image, K, fdw);
	//hold = dot_product(filter, image, K);
	//printf("%d, %f\n", col + row*N, hold);
	//C[row * K + col] = read_mat_from_xillybus(fdr);
        //read(fdr, (void *) &C[row * K + col], sizeof(float));
      }
      //return;
    }
}

void array_to_file(float *vector, int size) {
    FILE *fp; 

    char buf[20];
    sprintf(buf, "output%d.txt", convLayerNum);

    fp = fopen(buf, "w+");

    for (int i = 0; i < size; i++) {
        fprintf(fp, "%d, %f\n", i, vector[i]);
    }

    fclose(fp);
}

template<>
void caffe_cpu_gemm<float>(const CBLAS_TRANSPOSE TransA,
    const CBLAS_TRANSPOSE TransB, const int M, const int N, const int K,
    const float alpha, const float* A, const float* B, const float beta,
    float* C) {
#ifdef USE_FPGA
  printf("We're doing matrix multiplication! M %d  K %d   N %d\n", M, K, N);
  //printf("alpha %d   beta %d\n", alpha, beta);
  // Matrix A and B are not transposed

  /*
  C := alpha*op(A)*op(B) + beta*C,

  where:

  op(X) is one of op(X) = X, or op(X) = XT, or op(X) = XH,

  alpha and beta are scalars,

  A, B and C are matrices:

  op(A) is an m-by-k matrix,

  op(B) is a k-by-n matrix,

  C is an m-by-n matrix.
  */

  // This function is called in a couple different spots
  // For now we only care about the filter being colvolved with the image
  // (calls from forward_cpu_gemm)
  //if (convLayerNum % 2 == 0 && convLayerNum < 52)

  printf("N convLayerNum %d\n", convLayerNum);

  if (convLayerNum % 2 == 0 && convLayerNum < 52)
  {
      if (fd_r == 0) {
          fd_r = open("/dev/xillybus_read_32", O_RDONLY);
          fd_w = open("/dev/xillybus_write_32", O_WRONLY);
      }

    if (fd_w < 0)
    {
      printf("Failed to open Xillybus device");
      exit(1);
    }

    float *temp;
    std::thread write_thread(writer, fd_w, A, B, C, M, N, K);
    std::thread read_thread(reader, fd_r, C, M, N, K);

    write_thread.join();
    read_thread.join();

    //array_to_file(C, M*N);
  }
  else
  {
      int lda = (TransA == CblasNoTrans) ? K : M;
      int ldb = (TransB == CblasNoTrans) ? N : K;

      cblas_sgemm(CblasRowMajor, TransA, TransB, M, N, K, alpha, A, lda, B, ldb, beta, C, N);
  }
  
  convLayerNum++;

#else // CPU only
  int lda = (TransA == CblasNoTrans) ? K : M;
  int ldb = (TransB == CblasNoTrans) ? N : K;

  cblas_sgemm(CblasRowMajor, TransA, TransB, M, N, K, alpha, A, lda, B, ldb, beta, C, N);
#endif 
}

template<>
void caffe_cpu_gemm<double>(const CBLAS_TRANSPOSE TransA,
    const CBLAS_TRANSPOSE TransB, const int M, const int N, const int K,
    const double alpha, const double* A, const double* B, const double beta,
    double* C) {
  printf("We're doing matrix multiplication! M %d  K %d   N %d\n", M, K, N);
  int lda = (TransA == CblasNoTrans) ? K : M;
  int ldb = (TransB == CblasNoTrans) ? N : K;
  cblas_dgemm(CblasRowMajor, TransA, TransB, M, N, K, alpha, A, lda, B,
	ldb, beta, C, N);
  
}

template <>
void caffe_cpu_gemv<float>(const CBLAS_TRANSPOSE TransA, const int M,
    const int N, const float alpha, const float* A, const float* x,
    const float beta, float* y) {
  cblas_sgemv(CblasRowMajor, TransA, M, N, alpha, A, N, x, 1, beta, y, 1);
}

template <>
void caffe_cpu_gemv<double>(const CBLAS_TRANSPOSE TransA, const int M,
    const int N, const double alpha, const double* A, const double* x,
    const double beta, double* y) {
  cblas_dgemv(CblasRowMajor, TransA, M, N, alpha, A, N, x, 1, beta, y, 1);
}

template <>
void caffe_axpy<float>(const int N, const float alpha, const float* X,
    float* Y) { cblas_saxpy(N, alpha, X, 1, Y, 1); }

template <>
void caffe_axpy<double>(const int N, const double alpha, const double* X,
    double* Y) { cblas_daxpy(N, alpha, X, 1, Y, 1); }

template <typename Dtype>
void caffe_set(const int N, const Dtype alpha, Dtype* Y) {
  if (alpha == 0) {
    memset(Y, 0, sizeof(Dtype) * N);  // NOLINT(caffe/alt_fn)
    return;
  }
  for (int i = 0; i < N; ++i) {
    Y[i] = alpha;
  }
}

template void caffe_set<int>(const int N, const int alpha, int* Y);
template void caffe_set<float>(const int N, const float alpha, float* Y);
template void caffe_set<double>(const int N, const double alpha, double* Y);

template <>
void caffe_add_scalar(const int N, const float alpha, float* Y) {
  for (int i = 0; i < N; ++i) {
    Y[i] += alpha;
  }
}

template <>
void caffe_add_scalar(const int N, const double alpha, double* Y) {
  for (int i = 0; i < N; ++i) {
    Y[i] += alpha;
  }
}

template <typename Dtype>
void caffe_copy(const int N, const Dtype* X, Dtype* Y) {
  if (X != Y) {
    if (Caffe::mode() == Caffe::GPU) {
#ifndef CPU_ONLY
      // NOLINT_NEXT_LINE(caffe/alt_fn)
      CUDA_CHECK(cudaMemcpy(Y, X, sizeof(Dtype) * N, cudaMemcpyDefault));
#else
      NO_GPU;
#endif
    } else {
      memcpy(Y, X, sizeof(Dtype) * N);  // NOLINT(caffe/alt_fn)
    }
  }
}

template void caffe_copy<int>(const int N, const int* X, int* Y);
template void caffe_copy<unsigned int>(const int N, const unsigned int* X,
    unsigned int* Y);
template void caffe_copy<float>(const int N, const float* X, float* Y);
template void caffe_copy<double>(const int N, const double* X, double* Y);

template <>
void caffe_scal<float>(const int N, const float alpha, float *X) {
  cblas_sscal(N, alpha, X, 1);
}

template <>
void caffe_scal<double>(const int N, const double alpha, double *X) {
  cblas_dscal(N, alpha, X, 1);
}

template <>
void caffe_cpu_axpby<float>(const int N, const float alpha, const float* X,
                            const float beta, float* Y) {
  cblas_saxpby(N, alpha, X, 1, beta, Y, 1);
}

template <>
void caffe_cpu_axpby<double>(const int N, const double alpha, const double* X,
                             const double beta, double* Y) {
  cblas_daxpby(N, alpha, X, 1, beta, Y, 1);
}

template <>
void caffe_add<float>(const int n, const float* a, const float* b,
    float* y) {
  vsAdd(n, a, b, y);
}

template <>
void caffe_add<double>(const int n, const double* a, const double* b,
    double* y) {
  vdAdd(n, a, b, y);
}

template <>
void caffe_sub<float>(const int n, const float* a, const float* b,
    float* y) {
  vsSub(n, a, b, y);
}

template <>
void caffe_sub<double>(const int n, const double* a, const double* b,
    double* y) {
  vdSub(n, a, b, y);
}

template <>
void caffe_mul<float>(const int n, const float* a, const float* b,
    float* y) {
  vsMul(n, a, b, y);
}

template <>
void caffe_mul<double>(const int n, const double* a, const double* b,
    double* y) {
  vdMul(n, a, b, y);
}

template <>
void caffe_div<float>(const int n, const float* a, const float* b,
    float* y) {
  vsDiv(n, a, b, y);
}

template <>
void caffe_div<double>(const int n, const double* a, const double* b,
    double* y) {
  vdDiv(n, a, b, y);
}

template <>
void caffe_powx<float>(const int n, const float* a, const float b,
    float* y) {
  vsPowx(n, a, b, y);
}

template <>
void caffe_powx<double>(const int n, const double* a, const double b,
    double* y) {
  vdPowx(n, a, b, y);
}

template <>
void caffe_sqr<float>(const int n, const float* a, float* y) {
  vsSqr(n, a, y);
}

template <>
void caffe_sqr<double>(const int n, const double* a, double* y) {
  vdSqr(n, a, y);
}

template <>
void caffe_sqrt<float>(const int n, const float* a, float* y) {
  vsSqrt(n, a, y);
}

template <>
void caffe_sqrt<double>(const int n, const double* a, double* y) {
  vdSqrt(n, a, y);
}

template <>
void caffe_exp<float>(const int n, const float* a, float* y) {
  vsExp(n, a, y);
}

template <>
void caffe_exp<double>(const int n, const double* a, double* y) {
  vdExp(n, a, y);
}

template <>
void caffe_log<float>(const int n, const float* a, float* y) {
  vsLn(n, a, y);
}

template <>
void caffe_log<double>(const int n, const double* a, double* y) {
  vdLn(n, a, y);
}

template <>
void caffe_abs<float>(const int n, const float* a, float* y) {
    vsAbs(n, a, y);
}

template <>
void caffe_abs<double>(const int n, const double* a, double* y) {
    vdAbs(n, a, y);
}

unsigned int caffe_rng_rand() {
  return (*caffe_rng())();
}

template <typename Dtype>
Dtype caffe_nextafter(const Dtype b) {
  return boost::math::nextafter<Dtype>(
      b, std::numeric_limits<Dtype>::max());
}

template
float caffe_nextafter(const float b);

template
double caffe_nextafter(const double b);

template <typename Dtype>
void caffe_rng_uniform(const int n, const Dtype a, const Dtype b, Dtype* r) {
  CHECK_GE(n, 0);
  CHECK(r);
  CHECK_LE(a, b);
  boost::uniform_real<Dtype> random_distribution(a, caffe_nextafter<Dtype>(b));
  boost::variate_generator<caffe::rng_t*, boost::uniform_real<Dtype> >
      variate_generator(caffe_rng(), random_distribution);
  for (int i = 0; i < n; ++i) {
    r[i] = variate_generator();
  }
}

template
void caffe_rng_uniform<float>(const int n, const float a, const float b,
                              float* r);

template
void caffe_rng_uniform<double>(const int n, const double a, const double b,
                               double* r);

template <typename Dtype>
void caffe_rng_gaussian(const int n, const Dtype a,
                        const Dtype sigma, Dtype* r) {
  CHECK_GE(n, 0);
  CHECK(r);
  CHECK_GT(sigma, 0);
  boost::normal_distribution<Dtype> random_distribution(a, sigma);
  boost::variate_generator<caffe::rng_t*, boost::normal_distribution<Dtype> >
      variate_generator(caffe_rng(), random_distribution);
  for (int i = 0; i < n; ++i) {
    r[i] = variate_generator();
  }
}

template
void caffe_rng_gaussian<float>(const int n, const float mu,
                               const float sigma, float* r);

template
void caffe_rng_gaussian<double>(const int n, const double mu,
                                const double sigma, double* r);

template <typename Dtype>
void caffe_rng_bernoulli(const int n, const Dtype p, int* r) {
  CHECK_GE(n, 0);
  CHECK(r);
  CHECK_GE(p, 0);
  CHECK_LE(p, 1);
  boost::bernoulli_distribution<Dtype> random_distribution(p);
  boost::variate_generator<caffe::rng_t*, boost::bernoulli_distribution<Dtype> >
      variate_generator(caffe_rng(), random_distribution);
  for (int i = 0; i < n; ++i) {
    r[i] = variate_generator();
  }
}

template
void caffe_rng_bernoulli<double>(const int n, const double p, int* r);

template
void caffe_rng_bernoulli<float>(const int n, const float p, int* r);

template <typename Dtype>
void caffe_rng_bernoulli(const int n, const Dtype p, unsigned int* r) {
  CHECK_GE(n, 0);
  CHECK(r);
  CHECK_GE(p, 0);
  CHECK_LE(p, 1);
  boost::bernoulli_distribution<Dtype> random_distribution(p);
  boost::variate_generator<caffe::rng_t*, boost::bernoulli_distribution<Dtype> >
      variate_generator(caffe_rng(), random_distribution);
  for (int i = 0; i < n; ++i) {
    r[i] = static_cast<unsigned int>(variate_generator());
  }
}

template
void caffe_rng_bernoulli<double>(const int n, const double p, unsigned int* r);

template
void caffe_rng_bernoulli<float>(const int n, const float p, unsigned int* r);

template <>
float caffe_cpu_strided_dot<float>(const int n, const float* x, const int incx,
    const float* y, const int incy) {
  return cblas_sdot(n, x, incx, y, incy);
}

template <>
double caffe_cpu_strided_dot<double>(const int n, const double* x,
    const int incx, const double* y, const int incy) {
  return cblas_ddot(n, x, incx, y, incy);
}

template <typename Dtype>
Dtype caffe_cpu_dot(const int n, const Dtype* x, const Dtype* y) {
  return caffe_cpu_strided_dot(n, x, 1, y, 1);
}

template
float caffe_cpu_dot<float>(const int n, const float* x, const float* y);

template
double caffe_cpu_dot<double>(const int n, const double* x, const double* y);

template <>
float caffe_cpu_asum<float>(const int n, const float* x) {
  return cblas_sasum(n, x, 1);
}

template <>
double caffe_cpu_asum<double>(const int n, const double* x) {
  return cblas_dasum(n, x, 1);
}

template <>
void caffe_cpu_scale<float>(const int n, const float alpha, const float *x,
                            float* y) {
  cblas_scopy(n, x, 1, y, 1);
  cblas_sscal(n, alpha, y, 1);
}

template <>
void caffe_cpu_scale<double>(const int n, const double alpha, const double *x,
                             double* y) {
  cblas_dcopy(n, x, 1, y, 1);
  cblas_dscal(n, alpha, y, 1);
}

inline float read_mat_from_xillybus(int fdr) {

  float output;

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-result" 
  read(fdr, (void *) &output, sizeof(float));
#pragma GCC diagnostic pop 

  return output;
}

inline void write_mat_to_xillybus(float* vector, int size, int fdw) {
    int rc, donebytes;
    size_t packet_size;
    char *buf;

    buf = (char *) vector;
    donebytes = 0;
    packet_size = sizeof(float)*size;
    //packet_size = size;

    while (donebytes < packet_size) {
        rc = write(fdw, buf + donebytes, packet_size - donebytes);

        if ((rc < 0) && (errno == EINTR))
            continue;

        if (rc <= 0) {
            perror("write() failed");
            exit(1);
        }

        donebytes += rc;
    }
}

}  // namespace caffe
