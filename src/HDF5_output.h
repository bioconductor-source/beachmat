#ifndef BEACHMAT_HDF5_OUTPUT_H
#define BEACHMAT_HDF5_OUTPUT_H

#include "beachmat.h"
#include "any_matrix.h"
#include "HDF5_utils.h"
#include "output_param.h"

namespace beachmat {

/*** Class definition ***/

template<typename T, class V>
class HDF5_output : public any_matrix {
public:
    HDF5_output(size_t, size_t, 
            size_t=output_param::DEFAULT_CHUNKDIM, 
            size_t=output_param::DEFAULT_CHUNKDIM, 
            int=output_param::DEFAULT_COMPRESS, 
            size_t=output_param::DEFAULT_STRLEN);
    ~HDF5_output();

    // Setters:    
    void insert_row(size_t, const T*, size_t, size_t);
    template<typename X>
    void insert_row(size_t, const X*, const H5::DataType&, size_t, size_t);

    void insert_col(size_t, const T*, size_t, size_t);
    template<typename X>
    void insert_col(size_t, const X*, const H5::DataType&, size_t, size_t);

    void insert_one(size_t, size_t, T*);

    template<typename X>
    void insert_col_indexed(size_t, size_t, const int*, const X*, const H5::DataType&); 

    // Getters:
    void extract_col(size_t, T*, size_t, size_t);
    template<typename X>
    void extract_col(size_t, X*, const H5::DataType&, size_t, size_t);

    void extract_row(size_t, T*, size_t, size_t);
    template<typename X>
    void extract_row(size_t, X*, const H5::DataType&, size_t, size_t);

    void extract_one(size_t, size_t, T*);

    // Other:
    Rcpp::RObject yield();

    matrix_type get_matrix_type() const;
protected:
    std::string fname, dname;

    H5::H5File hfile;
    H5::DataSet hdata;
    H5::DataSpace hspace, rowspace, colspace, onespace;
    hsize_t h5_start[2], col_count[2], row_count[2], one_count[2];

    H5::DataType default_type;
    void select_row(size_t, size_t, size_t);
    void select_col(size_t, size_t, size_t);
    void select_one(size_t, size_t);

    bool onrow, oncol;
    bool rowokay, colokay;
    bool largerrow, largercol;
    H5::FileAccPropList rowlist, collist;

    // These functions are defined for each realized matrix type separately.
    T get_empty() const;
    Rcpp::RObject get_firstval();

    // Objects for indexed access.
    std::vector<hsize_t> index_coords;
    H5::DataSpace index_space;   
};

/*** Constructor definition ***/

template<typename T, class V>
HDF5_output<T, V>::HDF5_output (size_t nr, size_t nc, size_t chunk_nr, size_t chunk_nc, int compress, size_t len) : any_matrix(nr, nc), 
        rowlist(H5::FileAccPropList::DEFAULT.getId()), collist(H5::FileAccPropList::DEFAULT.getId()) {

    // Pulling out settings.
    auto RTYPE=V().sexp_type();
    const Rcpp::Environment env=Rcpp::Environment::namespace_env("beachmat");
    Rcpp::Function fun=env["setupHDF5Matrix"];
    Rcpp::List collected=fun(Rcpp::IntegerVector::create(this->nrow, this->ncol), Rcpp::StringVector(translate_type(RTYPE)),
                             Rcpp::IntegerVector::create(chunk_nr, chunk_nc), compress);

    if (collected.size()!=4) { 
        throw std::runtime_error("output of setupHDF5Matrix should be a list of four elements");
    }
    fname=make_to_string(collected[0]);
    dname=make_to_string(collected[1]);
    Rcpp::IntegerVector r_chunks=collected[2];
    if (r_chunks.size()!=2) { 
        throw std::runtime_error("chunk dimensions should be an integer vector of length 2");
    }
    chunk_nr=r_chunks[0];
    chunk_nc=r_chunks[1];
    Rcpp::IntegerVector r_compress=collected[3];
    if (r_compress.size()!=1) {
        throw std::runtime_error("compression should be an integer scalar");
    }
    compress=r_compress[0];

    // Opening the file, setting the type and creating the data set.
    hfile.openFile(fname.c_str(), H5F_ACC_RDWR);
    default_type=set_HDF5_data_type(RTYPE, len);
    H5::DSetCreatPropList plist;
    const T empty=get_empty();
    plist.setFillValue(default_type, &empty);
        
    // Setting the chunk dimensions if not contiguous and has non-zero dimensions.
    if (compress>0 && nr && nc) {
        std::vector<hsize_t> chunk_dims(2);
        chunk_dims[0]=chunk_nc; // flipping them, as rhdf5 internally transposes it.
        chunk_dims[1]=chunk_nr;
        plist.setLayout(H5D_CHUNKED);
        plist.setChunk(2, chunk_dims.data());
        plist.setDeflate(compress);
    } else {
        plist.setLayout(H5D_CONTIGUOUS);
    }

    std::vector<hsize_t> dims(2);
    dims[0]=this->ncol; // Setting the dimensions (0 is column, 1 is row; internally transposed).
    dims[1]=this->nrow; 

    hspace.setExtentSimple(2, dims.data());
    hdata=hfile.createDataSet(dname.c_str(), default_type, hspace, plist); 

    // Initializing the hsize_t[2] arrays.
    initialize_HDF5_size_arrays (this->nrow, this->ncol, 
            h5_start, col_count, row_count, 
            one_count, onespace);

    // Setting logical attributes.
    if (RTYPE==LGLSXP) {
        H5::StrType str_type(0, H5T_VARIABLE);
        H5::DataSpace att_space(1, one_count);
        H5::Attribute att = hdata.createAttribute("storage.mode", str_type, att_space);
        att.write(str_type, std::string("logical"));
    }

    // Setting the chunk cache parameters.
    calc_HDF5_chunk_cache_settings(this->nrow, this->ncol, hdata.getCreatePlist(), default_type, 
            onrow, oncol, rowokay, colokay, largerrow, largercol, rowlist, collist);
    return;
}

template<typename T, class V>
HDF5_output<T, V>::~HDF5_output() {}

/*** Defining selections ***/

template<typename T, class V>
void HDF5_output<T, V>::select_col(size_t c, size_t first, size_t last) {
    check_colargs(c, first, last);
    reopen_HDF5_file_by_dim(fname, dname,
            hfile, hdata, H5F_ACC_RDWR, collist, 
            oncol, onrow, largerrow, colokay);
    HDF5_select_col(c, first, last, col_count, h5_start, colspace, hspace);
    return;
}

template<typename T, class V>
void HDF5_output<T, V>::select_row(size_t r, size_t first, size_t last) {
    check_rowargs(r, first, last);
    reopen_HDF5_file_by_dim(fname, dname, 
            hfile, hdata, H5F_ACC_RDWR, rowlist, 
            onrow, oncol, largercol, rowokay);
    HDF5_select_row(r, first, last, row_count, h5_start, rowspace, hspace);
    return;
}

template<typename T, class V>
void HDF5_output<T, V>::select_one(size_t r, size_t c) {
    check_oneargs(r, c);
    HDF5_select_one(r, c, one_count, h5_start, hspace);
    return;
}

/*** Setter methods ***/

template<typename T, class V>
template<typename X>
void HDF5_output<T, V>::insert_col(size_t c, const X* in, const H5::DataType& HDT, size_t first, size_t last) {
    select_col(c, first, last);
    hdata.write(in, HDT, colspace, hspace);
    return;
}

template<typename T, class V>
void HDF5_output<T, V>::insert_col(size_t c, const T* in, size_t first, size_t last) {
    insert_col(c, in, default_type, first, last);
    return;
}

template<typename T, class V>
template<typename X>
void HDF5_output<T, V>::insert_row(size_t c, const X* in, const H5::DataType& HDT, size_t first, size_t last) {
    select_row(c, first, last);
    hdata.write(in, HDT, rowspace, hspace);
    return;
}

template<typename T, class V>
void HDF5_output<T, V>::insert_row(size_t c, const T* in, size_t first, size_t last) {
    insert_row(c, in, default_type, first, last);
    return;
}

template<typename T, class V>
void HDF5_output<T, V>::insert_one(size_t r, size_t c, T* in) {
    select_one(r, c);
    hdata.write(in, default_type, onespace, hspace);
    return;
}

template<typename T, class V>
template<typename X>
void HDF5_output<T, V>::insert_col_indexed(size_t c, size_t n, const int* idx, const X* val, const H5::DataType& HDT) {
    if (!n) { return; }

    if (index_coords.size()/2 < n) {
        size_t N=std::max(n, this->nrow);
        index_coords.resize(N*2);
        hsize_t tmp_N=N;
        index_space.setExtentSimple(1, &tmp_N);
    }

    // Setting up the coordinate space.
    select_col(c, 0, 0);
    auto wsIt=index_coords.begin();
    for (size_t i=0; i<n; ++i, ++idx) {
        (*wsIt)=c;
        ++wsIt;
        (*wsIt)=*idx;
        ++wsIt;
    }
    hspace.selectElements(H5S_SELECT_SET, n, index_coords.data());

    // Performing a write operation.
    hsize_t tmp_count=n, tmp_start=0;
    index_space.selectHyperslab(H5S_SELECT_SET, &tmp_count, &tmp_start);
    hdata.write(val, HDT, index_space, hspace);
    return;
}

/*** Getter methods ***/

template<typename T, class V>
template<typename X>
void HDF5_output<T, V>::extract_row(size_t r, X* out, const H5::DataType& HDT, size_t first, size_t last) { 
    select_row(r, first, last);
    hdata.read(out, HDT, rowspace, hspace);
    return;
} 

template<typename T, class V>
void HDF5_output<T, V>::extract_row(size_t r, T* out, size_t first, size_t last) { 
    extract_row(r, out, default_type, first, last);
    return;
} 

template<typename T, class V>
template<typename X>
void HDF5_output<T, V>::extract_col(size_t c, X* out, const H5::DataType& HDT, size_t first, size_t last) { 
    select_col(c, first, last);
    hdata.read(out, HDT, colspace, hspace);
    return;
}

template<typename T, class V>
void HDF5_output<T, V>::extract_col(size_t c, T* out, size_t first, size_t last) { 
    extract_col(c, out, default_type, first, last);
    return;
}

template<typename T, class V>
void HDF5_output<T, V>::extract_one(size_t r, size_t c, T* out) { 
    select_one(r, c);
    hdata.read(out, default_type, onespace, hspace);
    return;
}

/*** Output function ***/

template<typename T, class V>
Rcpp::RObject HDF5_output<T, V>::yield() {
    std::string seedclass="HDF5ArraySeed";
    Rcpp::S4 h5seed(seedclass);

    // Assigning to slots.
    if (!h5seed.hasSlot("filepath")) {
        throw_custom_error("missing 'filepath' slot in ", seedclass, " object");
    }
    h5seed.slot("filepath") = fname;

    if (!h5seed.hasSlot("name")) {
        throw_custom_error("missing 'name' slot in ", seedclass, " object");
    }
    h5seed.slot("name") = dname;

    if (!h5seed.hasSlot("dim")) {
        throw_custom_error("missing 'dim' slot in ", seedclass, " object");
    }
    h5seed.slot("dim") = Rcpp::IntegerVector::create(this->nrow, this->ncol);

    if (!h5seed.hasSlot("first_val")) {
        throw_custom_error("missing 'first_val' slot in ", seedclass, " object");
    }
    if (this->nrow && this->ncol) { 
        h5seed.slot("first_val") = get_firstval();
    } else {
        h5seed.slot("first_val") = V(0); // empty vector.
    }

    // Assigning the seed to the HDF5Matrix.
    std::string matclass="HDF5Matrix";
    Rcpp::S4 h5mat(matclass);
    if (!h5mat.hasSlot("seed")) {
        throw_custom_error("missing 'seed' slot in ", matclass, " object");
    }
    h5mat.slot("seed") = h5seed;
    return Rcpp::RObject(h5mat);
}

template<typename T, class V>
matrix_type HDF5_output<T, V>::get_matrix_type() const {
    return HDF5;
}

}

#endif
