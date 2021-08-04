#include "ExodusIO.hpp"
#include <Teuchos_CommandLineProcessor.hpp>
#include <BelosTpetraAdapter.hpp>
#include <BelosSolverFactory.hpp>
#include <Tpetra_MultiVector.hpp>
#include <Ifpack2_Factory.hpp>

/*
    MueLu crashes in Amesos' 'transpose' function, so we use IFPACK2 instead.
*/

void printCrsMatrix(const Teuchos::RCP<const Tpetra::CrsMatrix<>> matrix, bool sparse=true) {
    auto rank = Tpetra::getDefaultComm()->getRank();
    auto ranks = Tpetra::getDefaultComm()->getSize();
    auto rows = matrix->getGlobalNumRows();
    auto map = matrix->getRowMap();

    for (int row = 0; row < rows; row++) {
        if (map->isNodeGlobalElement(row)) {
            std::cout << "Process #:" << rank << " [";
            Teuchos::Array<Tpetra::CrsMatrix<>::global_ordinal_type> cols(matrix->getNumEntriesInGlobalRow(row));
            Teuchos::Array<Tpetra::CrsMatrix<>::scalar_type> vals(matrix->getNumEntriesInGlobalRow(row));
            size_t sz;
            matrix->getGlobalRowCopy(row, cols, vals, sz);
            for (int i = 0; i < cols.size(); i++) {
                if (i) std::cout << ",";
                std::cout << "(" << i << "," << vals[i] << ")";
            }
            std::cout << "]" << std::endl;
            std::flush(std::cout);
        }
        Teuchos::barrier(*Tpetra::getDefaultComm());
    }
}

void printMultiVector(const Teuchos::RCP<const Tpetra::MultiVector<>> X) {
    auto rank = Tpetra::getDefaultComm()->getRank();
    auto ranks = Tpetra::getDefaultComm()->getSize();
    auto N = X->getGlobalLength();
    auto map = X->getMap();
    Teuchos::barrier(*Tpetra::getDefaultComm());
    for (int col = 0; col < X->getNumVectors(); col++) {
        if (rank == 0) {
            std::cout << "[" << std::endl;
            std::flush(std::cout);
        }
        auto vec = X->getVector(col);
        auto copy = vec->get1dView();

        for (int row = 0; row < vec->getGlobalLength(); row++) {
            if (vec->getMap()->isNodeGlobalElement(row)) {
                std::cout << "\t[";
                std::cout << copy[vec->getMap()->getLocalElement(row)];
                std::cout << "]" << std::endl;
                std::flush(std::cout);
            }
            Teuchos::barrier(*Tpetra::getDefaultComm());
        }
        Teuchos::barrier(*Tpetra::getDefaultComm());
        if (rank == 0) std::cout << "]" << std::endl;
    }
}

// Solves for ax=b
void belosSolver(const Teuchos::RCP<Tpetra::CrsMatrix<>> _A, const Teuchos::RCP<Tpetra::MultiVector<> > X, const Teuchos::RCP<const Tpetra::MultiVector<> > B, size_t numIterations, double tolerance) {
    Teuchos::RCP<Tpetra::Operator<>> A = _A;
    Teuchos::RCP<Tpetra::Operator<>> _M;
    Teuchos::RCP<const Tpetra::Operator<>> M;
    Teuchos::ParameterList plist;
    Ifpack2::Factory ifpackFactory;
    Teuchos::RCP<Ifpack2::Preconditioner<>> prec = ifpackFactory.create("ILUT", _A.getConst());
    prec->setParameters (plist);
    prec->initialize();
    prec->compute();
    M = prec.getConst();
    
    // Set up the solver
    auto solverOptions = Teuchos::rcp(new Teuchos::ParameterList());
    solverOptions->set ("Maximum Iterations", (int) numIterations);
    solverOptions->set ("Convergence Tolerance", tolerance);
    
    Belos::SolverFactory<double, Tpetra::MultiVector<>, Tpetra::Operator<>> factory;
    Teuchos::RCP<Belos::SolverManager<double, Tpetra::MultiVector<>, Tpetra::Operator<>>> solver = factory.create("GMRES", solverOptions);
    
    auto problem = Teuchos::rcp(new Belos::LinearProblem<double, Tpetra::MultiVector<>, Tpetra::Operator<>>(A, X, B));
    // problem->setRightPrec(M);
    problem->setProblem();
    solver->setProblem(problem);
    Belos::ReturnType result = solver->solve();
    
    // Ask the solver how many iterations the last solve() took.
    const int numIters = solver->getNumIters();

    const double tTolerance = solver->achievedTol();
    if (result == Belos::Converged) {
        if (Tpetra::getDefaultComm()->getRank() == 0) {
            std::cout << "The Belos solve took " << numIters << " iteration(s) to reach "
            "a relative residual tolerance of " << tTolerance << "." << std::endl;
        }
    } else {
        if (Tpetra::getDefaultComm()->getRank() == 0) {
            std::cout << "The Belos solve took " << numIters << " iteration(s), but did not converge. Achieved tolerance = "
                    << tTolerance << "." << std::endl;
        }
    }
    printMultiVector(X);
}

int main(int argc, char *argv[]) {
    Tpetra::ScopeGuard tscope(&argc, &argv);
    {
        Teuchos::CommandLineProcessor cmdp(false, false);
        std::string inputFile = "";
        bool verbose = false;
        size_t numIterations = 300;
        size_t reportAfterIterations = 10;
        double tolerance = 1e-14;
        cmdp.setOption("input", &inputFile, "Exodus file to decompose", true);
        cmdp.setOption("verbose", "no-verbose", &verbose, "Whether or not to be verbose with output (usefulf or debugging) [default=false]");
        cmdp.setOption("iterations", &numIterations, "Maximum number of iterations that the solver will run for [default=300]");
        cmdp.setOption("reportAfterIterations", &reportAfterIterations, "Number of iterations between checks for convergence, and if --verbose, how often information gets logged [default=10]");
        cmdp.setOption("tolerance", &tolerance, "Tolerance for convergence [default=1e-14]");
        cmdp.parse(argc, argv);
        auto comm = Tpetra::getDefaultComm();
        int rank = Teuchos::rank(*comm);
        if (inputFile.empty()) {
            if (rank == 0) std::cerr << "No input file was provided; use the '--input' parameter!" << std::endl;
            return EXIT_FAILURE;
        }
        if (Teuchos::size(*comm) == 1) {
            std::cerr << "Requires at least 2 MPI Processors for this Example!" << std::endl;
            return EXIT_FAILURE;
        }
        ExodusIO::IO io;
        if (!io.open(inputFile, true)) {
            std::cerr << "Process #" << rank << ": Failed to open input Exodus file '" << inputFile << "'" << std::endl;
            return EXIT_FAILURE;
        }

        Teuchos::RCP<Tpetra::CrsMatrix<>> ret;
        std::map<int, std::set<idx_t>> nodeSetMap;
        if (!io.getMatrix(&ret, nodeSetMap, verbose)) {
            std::cerr << "Process #" << rank <<  ": Failed to getMatrix!!" << std::endl;
            return EXIT_FAILURE;
        }

        auto ostr = Teuchos::VerboseObjectBase::getDefaultOStream();
        ret->describe(*ostr, verbose ? Teuchos::EVerbosityLevel::VERB_EXTREME : Teuchos::EVerbosityLevel::VERB_MEDIUM);
        printCrsMatrix(ret);

        // Invoke solver...
        // The linear equation being solved for is 'AX = B', where A is the laplacian matrix,
        // X is the solution (with initial randomized guess), and B is the desired values to converge to.
        Teuchos::RCP<Tpetra::MultiVector<>> X = Teuchos::rcp(new Tpetra::MultiVector<>(ret->getDomainMap(),1));
        Teuchos::RCP<Tpetra::MultiVector<>> B = Teuchos::rcp(new Tpetra::MultiVector<>(ret->getRangeMap(),1));
        B->putScalar(0);
        srand(time(NULL));
        X->randomize();
        printMultiVector(X);
        belosSolver(ret, X, B, numIterations, tolerance);
    }

    return 0;
}