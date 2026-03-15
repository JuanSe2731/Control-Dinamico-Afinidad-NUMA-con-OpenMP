#include <stdlib.h>
#include <omp.h>
#include <vector>
#include <iostream>

void createMAtrix(std::vector<std::vector<float>>& A, int n)
{
    for (int i = 0; i < n; i++){
        for (int j = 0; j < n; j++){
            A[i][j] = rand() % 100;
        }
    }

}

/*
void stencil2D(std::vector<std::vector<float>>& A, std::vector<std::vector<float>>& result, int n)
{
    //omp_set_num_threads(12);

    int nthreads = omp_get_num_threads();
    std::cout<<"Number of threads: "<<nthreads<<std::endl;

    #pragma omp parallel for collapse(2)
    for (int j = 1; j < n - 1; ++j) {
        for (int k = 1; k < n - 1; ++k) {
            result[j][k] =
                0.25f * A[j][k] +
                0.25f * A[j][k - 1] +  // izquierda
                0.25f * A[j][k + 1] +  // derecha
                0.25f * A[j - 1][k] +  // arriba
                0.25f * A[j + 1][k];   // abajo
        }
    }

}*/

void stencil2D(std::vector<std::vector<float>>& A, std::vector<std::vector<float>>& result, int n)
{
#pragma omp parallel
    {
#pragma omp single
        {
            std::cout << "Max threads available: " << omp_get_max_threads() << std::endl;
            std::cout << "Number of threads (actual): "<< omp_get_num_threads() << std::endl;
        }

#pragma omp for collapse(2)
        for (int j = 1; j < n - 1; ++j) {
            for (int k = 1; k < n - 1; ++k) {
                result[j][k] =
                    0.25f * A[j][k] +
                    0.25f * A[j][k - 1] +
                    0.25f * A[j][k + 1] +
                    0.25f * A[j - 1][k] +
                    0.25f * A[j + 1][k];
            }
        }
    }
}



int main(int argc, char* argv[])
{
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " N\n";
        return 1;
    }

    //omp_set_dynamic(0);

    //int nthreads = omp_get_num_threads();


    //Size of matrix
    int n = atoi(argv[1]);

    //Stencil

    std::vector<std::vector<float>> A(n, std::vector<float>(n));
    std::vector<std::vector<float>> result(n, std::vector<float>(n));

    createMAtrix(A,n);

    double timestart = omp_get_wtime();
    stencil2D(A, result, n);
    double timeend = omp_get_wtime();

    std::cout<<"Time elapsed: "<<timeend-timestart<<std::endl;




}