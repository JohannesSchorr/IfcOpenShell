#include "../ifcparse/IfcHdf5File.h"
#include "../ifcparse/IfcWrite.h"

#include <bitset>
#include <limits>
#include <H5pubconf.h>
#include <boost/lexical_cast.hpp>

#ifndef H5_HAVE_FILTER_DEFLATE
#pragma message("warning: HDF5 compression support is recommended")
#endif

static boost::optional< std::vector<Argument*> > no_instances = boost::none;

class type_mapper {
public:
	type_mapper();
	type_mapper(IfcParse::IfcFile* ifc_file, H5::H5File* hdf5_file, const IfcParse::Hdf5Settings& settings);

	H5::DataType* commit(H5::DataType* dt, const std::string& name);
	
	H5::DataType* operator()(const IfcParse::parameter_type* pt, const boost::optional< std::vector<Argument*> >& instances = no_instances, int dims = 0, const hsize_t* max_length = nullptr);
	H5::DataType* operator()(const IfcParse::select_type* pt, const boost::optional< std::vector<Argument*> >& instances = no_instances, int dims = 0, const hsize_t* max_length = nullptr);
	H5::CompType* operator()(const IfcParse::entity* e, const boost::optional< std::vector<Argument*> >& instances = no_instances, int dims = 0, const hsize_t* max_length = nullptr);
	H5::EnumType* operator()(const IfcParse::enumeration_type* en, const boost::optional< std::vector<Argument*> >& instances = no_instances, int dims = 0, const hsize_t* max_length = nullptr);
	
	void operator()();

	std::pair<std::string, const H5::DataType*> make_select_leaf(const IfcParse::declaration* decl, const boost::optional< std::vector<Argument*> >& instances);

private:
	IfcParse::Hdf5Settings settings_; 
	bool padded_;
	bool referenced_;
	
	IfcParse::IfcFile* ifc_file_;
	H5::H5File* hdf5_file_;
	H5::Group schema_group_;
	H5::DataType* instance_reference_;
	std::vector<const H5::DataType*> default_types_;
	std::vector<std::string> default_type_names_;
	std::vector<std::string> default_cpp_type_names_;
	std::vector<H5::DataType*> declared_types_;

	std::string flatten_aggregate_name(const IfcParse::parameter_type* at) const;
};

class enumeration_reference {
private:
	size_t index_;
public:
	explicit enumeration_reference(size_t index)
		: index_(index)
	{}

	operator size_t() const {
		return index_;
	}
};

class select_item {
private:
	IfcUtil::IfcBaseClass* data_;
public:
	explicit select_item(IfcUtil::IfcBaseClass* data)
		: data_(data)
	{}

	operator IfcUtil::IfcBaseClass*() const {
		return data_;
	}
};

template <typename T> struct is_hdf5_integral { static const bool value = false; };
template <> struct is_hdf5_integral <int>     { static const bool value =  true; };
template <> struct is_hdf5_integral <bool>    { static const bool value =  true; };
template <> struct is_hdf5_integral <boost::logic::tribool> { static const bool value = true; };
template <> struct is_hdf5_integral <float>   { static const bool value =  true; };
template <> struct is_hdf5_integral <double>  { static const bool value =  true; };
template <> struct is_hdf5_integral <enumeration_reference> { static const bool value = true; };

template <typename T> struct hdf5_datatype_for{};
template <> struct hdf5_datatype_for <int>    { static const H5T_class_t value = H5T_INTEGER; };
// Booleans and logicals are enumerations in HDF5 as well!
template <> struct hdf5_datatype_for <bool>   { static const H5T_class_t value = H5T_ENUM; };
template <> struct hdf5_datatype_for <boost::logic::tribool> { static const H5T_class_t value = H5T_ENUM; };
template <> struct hdf5_datatype_for <float>  { static const H5T_class_t value = H5T_FLOAT; };
template <> struct hdf5_datatype_for <double> { static const H5T_class_t value = H5T_FLOAT; };
template <> struct hdf5_datatype_for <enumeration_reference> { static const H5T_class_t value = H5T_ENUM; };

template <unsigned int T> struct uint_of_size {};
template <>               struct uint_of_size <1> { typedef  uint8_t type; };
template <>               struct uint_of_size <2> { typedef uint16_t type; };
template <>               struct uint_of_size <4> { typedef uint32_t type; };
template <>               struct uint_of_size <8> { typedef uint64_t type; };

template <unsigned int T> struct int_of_size {};
template <>               struct int_of_size <1> { typedef  int8_t type; };
template <>               struct int_of_size <2> { typedef int16_t type; };
template <>               struct int_of_size <4> { typedef int32_t type; };
template <>               struct int_of_size <8> { typedef int64_t type; };

template <unsigned int T> struct float_of_size {};
template <>               struct float_of_size <4> { typedef  float type; };
template <>               struct float_of_size <8> { typedef double type; };

template <typename T, unsigned int N> struct number_of_size {};
template <unsigned int N> struct number_of_size <int, N>    { typedef typename   int_of_size<N>::type type; };
template <unsigned int N> struct number_of_size <float, N>  { typedef typename float_of_size<N>::type type; };
template <unsigned int N> struct number_of_size <double, N> { typedef typename float_of_size<N>::type type; };

std::pair<H5T_class_t, H5T_class_t> compound_member_types_as_pair(H5::DataType& datatype) {
	if (datatype.getClass() != H5T_COMPOUND) {
		throw std::runtime_error("Expected a compound");
	}

	H5::CompType* compound = (H5::CompType*) &datatype;

	if (compound->getNmembers() != 2) {
		throw std::runtime_error("Expected a compound with two members");
	}

	return std::make_pair(
		compound->getMemberDataType(0).getClass(),
		compound->getMemberDataType(1).getClass());
}

bool is_select(H5::DataType& datatype) {
	if (datatype.getClass() != H5T_COMPOUND) {
		return false;
	}

	H5::CompType* compound = (H5::CompType*) &datatype;

	if (compound->getNmembers() < 1) {
		return false;
	}

	return compound->getMemberName(0) == "type_code";
}

void advance(void*& ptr, size_t n) {
	ptr = (uint8_t*)ptr + n;
}

template <typename T>
void write(void*& ptr, const T& t) {
	// *((T*)ptr) = t;
	memcpy(ptr, &t, sizeof(T));
	advance(ptr, sizeof(T));
}

template <>
void write(void*& ptr, const std::string& s) {
	// TFK: So we assume this is freed by h5 vlen reclaim?
	char* c = new char[s.size() + 1];
	strcpy(c, s.c_str());
	write(ptr, c);
}

template <typename T>
void write_number_of_size(void*& ptr, size_t n, T i) {
	void* old_ptr = ptr;
	if (std::is_floating_point<T>::value) {
		if (n == 4) {
			write(ptr, static_cast< float_of_size<4>::type > (i));
		} else if (n == 8) {
			write(ptr, static_cast< float_of_size<8>::type > (i));
		}
	} else if (std::numeric_limits<T>::is_signed) {
		if (n == 1) {
			write(ptr, static_cast< int_of_size<1>::type > (i));
		} else if (n == 2) {
			write(ptr, static_cast< int_of_size<2>::type > (i));
		} else if (n == 4) {
			write(ptr, static_cast< int_of_size<4>::type > (i));
		} else if (n == 8) {
			write(ptr, static_cast< int_of_size<8>::type > (i));
		}
	} else {
		// NB: enumeration_reference also ends up here
		if (n == 1) {
			write(ptr, static_cast< uint_of_size<1>::type > (i));
		} else if (n == 2) {
			write(ptr, static_cast< uint_of_size<2>::type > (i));
		} else if (n == 4) {
			write(ptr, static_cast< uint_of_size<4>::type > (i));
		} else if (n == 8) {
			write(ptr, static_cast< uint_of_size<8>::type > (i));
		}
	}
	if (old_ptr == ptr) {
		throw std::runtime_error("No value written");
	}
}

void write_number_of_size(void*& ptr, size_t n, boost::logic::tribool b) {
	int i = b.value == boost::logic::tribool::false_value
		? 0 : b.value == boost::logic::tribool::indeterminate_value
		? -1 : 1;
	write_number_of_size(ptr, n, i);
}

void write_string_of_size(void*& ptr, size_t n, const std::string& s) {
	memset(ptr, 0, n);
	memcpy(ptr, s.c_str(), s.size());
	advance(ptr, n);
}

void write_vlen_t(void*& ptr, size_t n_elements, void* vlen_data) {
	advance(ptr, HOFFSET(hvl_t, len));
	void* temp_ptr;
	write_number_of_size(temp_ptr = ptr, sizeof(size_t), n_elements);
	advance(ptr, HOFFSET(hvl_t, p));
	write(ptr, vlen_data);
}

#define CAST_AND_CALL(attr, type, fn) {IfcUtil::attr_type_to_cpp_type<type>::cpp_type v = *attr; fn(v);}
#define CHECK_CAST_AND_CALL(ty) else if (attr_->type() == ty) CAST_AND_CALL(attr_, ty, t)

class apply_attribute_visitor {
private:
	Argument* attr_;
	const IfcParse::entity::attribute* schema_attr_;
public:
	apply_attribute_visitor(Argument* attr, const IfcParse::entity::attribute* schema_attr)
		: attr_(attr)
		, schema_attr_(schema_attr)
	{}

	template <typename T>
	typename T::return_type apply(T& t) const {
		if (attr_->type() == IfcUtil::Argument_NULL) {
			t(boost::none);
		} else if (attr_->type() == IfcUtil::Argument_DERIVED) {
			throw std::runtime_error("Derived attributes should not be written to the file");
			// IfcWrite::IfcWriteArgument::Derived d;
			// t(d);
		}
		CHECK_CAST_AND_CALL(IfcUtil::Argument_INT)
		CHECK_CAST_AND_CALL(IfcUtil::Argument_BOOL)
		CHECK_CAST_AND_CALL(IfcUtil::Argument_TRIBOOL)
		CHECK_CAST_AND_CALL(IfcUtil::Argument_DOUBLE)
		CHECK_CAST_AND_CALL(IfcUtil::Argument_STRING)
		else if (attr_->type() == IfcUtil::Argument_BINARY) {
			throw std::runtime_error("Binary not supported at the moment");
		}
		else if (attr_->type() == IfcUtil::Argument_ENUMERATION) {
			std::string v = *attr_;
			const std::vector<std::string>& enum_values = schema_attr_->type_of_attribute()->as_named_type()->declared_type()->as_enumeration_type()->enumeration_items();
			size_t d = std::distance(enum_values.begin(), std::find(enum_values.begin(), enum_values.end(), v));
			enumeration_reference enum_ref(d);
			t(enum_ref);
		}
		CHECK_CAST_AND_CALL(IfcUtil::Argument_ENTITY_INSTANCE)

		CHECK_CAST_AND_CALL(IfcUtil::Argument_AGGREGATE_OF_INT)
		CHECK_CAST_AND_CALL(IfcUtil::Argument_AGGREGATE_OF_BOOL)
		CHECK_CAST_AND_CALL(IfcUtil::Argument_AGGREGATE_OF_TRIBOOL)
		CHECK_CAST_AND_CALL(IfcUtil::Argument_AGGREGATE_OF_DOUBLE)
		CHECK_CAST_AND_CALL(IfcUtil::Argument_AGGREGATE_OF_STRING)
		else if (attr_->type() == IfcUtil::Argument_AGGREGATE_OF_BINARY) {
			throw std::runtime_error("Not supported currently");
		}
		else if (attr_->type() == IfcUtil::Argument_AGGREGATE_OF_ENTITY_INSTANCE) {
			IfcUtil::attr_type_to_cpp_type<IfcUtil::Argument_AGGREGATE_OF_ENTITY_INSTANCE>::cpp_type v = *attr_;
			std::vector<IfcUtil::IfcBaseClass*> vec(v->begin(), v->end());
			t(vec);
		}

		CHECK_CAST_AND_CALL(IfcUtil::Argument_AGGREGATE_OF_AGGREGATE_OF_INT)
		CHECK_CAST_AND_CALL(IfcUtil::Argument_AGGREGATE_OF_AGGREGATE_OF_BOOL)
		CHECK_CAST_AND_CALL(IfcUtil::Argument_AGGREGATE_OF_AGGREGATE_OF_TRIBOOL)
		CHECK_CAST_AND_CALL(IfcUtil::Argument_AGGREGATE_OF_AGGREGATE_OF_DOUBLE)
		else if (attr_->type() == IfcUtil::Argument_AGGREGATE_OF_AGGREGATE_OF_ENTITY_INSTANCE) {
			IfcUtil::attr_type_to_cpp_type<IfcUtil::Argument_AGGREGATE_OF_AGGREGATE_OF_ENTITY_INSTANCE>::cpp_type v = *attr_;
			std::vector< std::vector<IfcUtil::IfcBaseClass*> > vec(v->begin(), v->end());
			t(vec);
		}

		else if (attr_->type() == IfcUtil::Argument_UNKNOWN) {
			auto aggregation = schema_attr_->type_of_attribute()->as_aggregation_type();
			if (aggregation == nullptr) {
				throw std::exception("Attribute of unknown type encountered, expected empty aggregate");
			}
			auto elem_type = aggregation->type_of_element();
			if (elem_type->as_named_type()) {
				auto entity = elem_type->as_named_type()->declared_type()->as_entity();
				if (entity == nullptr) {
					throw std::exception("Not implemented");
				}
				std::vector<IfcUtil::IfcBaseClass*> empty;
				t(empty);
			} else if (elem_type->as_simple_type()) {
				auto dt = elem_type->as_simple_type()->declared_type();
				if (dt == IfcParse::simple_type::integer_type) {
					std::vector<int> empty;
					t(empty);
				} else {
					throw std::exception("Not implemented");
				}
			} else {
				throw std::exception("Not implemented");
			}
		}		
	}
};

class sorted_instance_locator {
private:
	H5::H5File& hdf5_file_;
	IfcParse::IfcFile& file_;
	std::vector< IfcSchema::Type::Enum > dataset_names_;
	std::vector< std::vector<IfcUtil::IfcBaseEntity*>* > cache_;
	bool referenced_;

public:	
	sorted_instance_locator(H5::H5File& hdf5_file, IfcParse::IfcFile& file, bool referenced)
		: hdf5_file_(hdf5_file)
		, file_(file)
		, referenced_(referenced)
	{
		std::set<IfcSchema::Type::Enum> dataset_names_temp;
		for (auto it = file_.begin(); it != file_.end(); ++it) {
			dataset_names_temp.insert(it->second->declaration().type());
		}
		dataset_names_.assign(dataset_names_temp.begin(), dataset_names_temp.end());
		std::sort(dataset_names_.begin(), dataset_names_.end());

		cache_.resize(IfcSchema::Type::UNDEFINED);

		if (referenced_) {
			// Why not always populate?
			const IfcSchema::Type::Enum begin = IfcSchema::Type::Ifc2DCompositeCurve;
			const IfcSchema::Type::Enum end = IfcSchema::Type::UNDEFINED;
			for (int i = begin; i != end; i++) {
				instances((IfcSchema::Type::Enum)i);
			}
		}
	}

	typedef std::vector< IfcSchema::Type::Enum >::const_iterator const_iterator;
	const_iterator begin() const { return dataset_names_.begin(); }
	const_iterator end() const { return dataset_names_.end(); }

	const std::vector<IfcUtil::IfcBaseEntity*>& instances(IfcSchema::Type::Enum t) {
		if (cache_[t] == nullptr) {
			auto li = file_.entitiesByType(t);
			std::vector<IfcUtil::IfcBaseEntity*>* vs = cache_[t] = new std::vector<IfcUtil::IfcBaseEntity*>();
			if (li) {
				vs->reserve(li->size());

				for (auto jt = li->begin(); jt != li->end(); ++jt) {
					if ((*jt)->declaration().type() == t) {
						vs->push_back(static_cast<IfcUtil::IfcBaseEntity*>(*jt));
					}
				}

				std::sort(vs->begin(), vs->end(), [](IfcUtil::IfcBaseEntity* i1, IfcUtil::IfcBaseEntity* i2) {
					return i1->data().id() < i2->data().id();
				});
			}
		}
		return *cache_[t];
	}

	std::string path(int dsidx) const {
		return IfcSchema::Type::ToString(dataset_names_[dsidx]);
	}

	std::pair<int, int> operator()(IfcUtil::IfcBaseClass* v) {
		if (IfcSchema::Type::IsSimple(v->declaration().type())) {
			throw std::exception("Simple type not expected here");
		}

		IfcSchema::Type::Enum t = v->declaration().type();
		auto tt = std::lower_bound(dataset_names_.begin(), dataset_names_.end(), t);
		int a = std::distance(dataset_names_.begin(), tt);
		int b;

		const std::vector<IfcUtil::IfcBaseEntity*>& insts = instances(t);

		auto it = std::lower_bound(insts.begin(), insts.end(), v, [](IfcUtil::IfcBaseClass* other, IfcUtil::IfcBaseClass* val) {
			// An ordering based on ID is not equivalent to an ordering based on pointer address!
			return other->data().id() < val->data().id();
		});

		if (it == insts.end()) {
			throw std::runtime_error("Unable to find instance");
		}

		b = std::distance(insts.begin(), it);
		
		return std::make_pair(a, b);
	}
};

class pointer_increment_assert {
	void*& ptr_reference_;
	uint8_t* ptr_initial_;
	size_t datatype_size_;
public:
	pointer_increment_assert(void*& ptr_reference, size_t datatype_size)
		: ptr_reference_(ptr_reference)
		, ptr_initial_(static_cast<uint8_t*>(ptr_reference))
		, datatype_size_(datatype_size)
	{}

	~pointer_increment_assert() noexcept(false) {
		if (ptr_initial_ + datatype_size_ != ptr_reference_) {
			throw std::runtime_error("Incorrect amount of bytes written");
		}
	}
};

class default_value_visitor {
private:
	H5::DataType& datatype_;

public:
	default_value_visitor(H5::DataType& datatype)
		: datatype_(datatype)
	{}

	void operator()(void*& ptr) {
		switch (datatype_.getClass()) {
		case H5T_INTEGER:
			write_number_of_size(ptr, datatype_.getSize(), 0);
			break;
		case H5T_FLOAT:
			write_number_of_size(ptr, datatype_.getSize(), 0.);
			break;
		case H5T_STRING:
			if (datatype_ == H5::StrType(H5::PredType::C_S1, H5T_VARIABLE)) {
				// write(ptr, new char(0));
				memset(ptr, 0, sizeof(char*));
				advance(ptr, sizeof(char*));
			} else {
				write_string_of_size(ptr, datatype_.getSize(), "");
			}
			break;
		case H5T_COMPOUND:
		{
			H5::CompType* compound = (H5::CompType*) &datatype_;
			for (int i = 0; i < compound->getNmembers(); ++i) {
				H5::DataType attr_type = compound->getMemberDataType(i);
				default_value_visitor visitor(attr_type);
				visitor(ptr);
			}
			break;
		}
		case H5T_REFERENCE:
			// A bit hard to figure out, but apparently a null-reference is simply zeros
			// https://github.com/h5py/h5py/blob/e611b7ca47e49d83e908d0368d2d9ced224a9de0/h5py/h5r.pyx#L167
			if (datatype_.getSize() != sizeof(hdset_reg_ref_t)) {
				throw std::runtime_error("Unexpected datatype size for reference");
			}
			memset(ptr, 0, sizeof(hdset_reg_ref_t));
			advance(ptr, sizeof(hdset_reg_ref_t));
			break;
		case H5T_ENUM:
			write_number_of_size(ptr, datatype_.getSize(), 0);
			break;
		case H5T_VLEN:
		{
			// H5::VarLenType* vlen = (H5::VarLenType*) &datatype_;
			// Does this work?
			write_vlen_t(ptr, 0, nullptr);
			break;
		}
		case H5T_ARRAY:
		{
			H5::ArrayType* arr = (H5::ArrayType*) &datatype_;
			size_t ndims = arr->getArrayNDims();
			hsize_t* array_dims = new hsize_t[ndims];
			arr->getArrayDims(array_dims);
			if (ndims != 1) {
				throw std::runtime_error("Not implemented");
			}
			H5::DataType super = arr->getSuper();
			default_value_visitor visitor(super);
			for (hsize_t i = 0; i < array_dims[0]; ++i) {
				visitor(ptr);
			}
			break;
		}
		default:
			throw std::runtime_error("Unexpected type encountered");
		}
	}
};

// template <typename LocatorT>
class write_visit {
private:
	H5::H5File& file_;
	bool padded_;
	bool referenced_;
	type_mapper& type_mapper_;

public:
	typedef sorted_instance_locator locator_type;

	locator_type& instance_locator_;

	write_visit(H5::H5File& file, bool padded, bool referenced, locator_type& instance_locator, type_mapper& type_mapper)
		: file_(file)
		, padded_(padded)
		, referenced_(referenced)
		, instance_locator_(instance_locator)
		, type_mapper_(type_mapper)
	{}

	template <typename T, typename std::enable_if<is_hdf5_integral<T>::value, T>::type* = nullptr>
	void visit(void*& ptr, H5::DataType& datatype, T& v) {
		if (datatype.getClass() != hdf5_datatype_for<T>::value) {
			throw std::runtime_error("Datatype and value do not match");
		}
		write_number_of_size(ptr, datatype.getSize(), v);
	}

	void visit(void*& ptr, H5::DataType& datatype, const boost::none_t&) {
		// TODO: Do we need to do sth based on datatype here, eg. allocate vlen/char* if necessary?
		memset(ptr, 0, datatype.getSize());
		advance(ptr, datatype.getSize());
	}
	
	// template <>
	void visit(void*& ptr, H5::DataType& datatype, std::string& v) {
		pointer_increment_assert _(ptr, datatype.getSize());
		if (padded_) {
			if (compound_member_types_as_pair(datatype) != std::make_pair(H5T_INTEGER, H5T_STRING)) {
				throw std::runtime_error("Datatype and value do not match");
			}

			H5::CompType* compound = (H5::CompType*) &datatype;
			H5::IntType int_type = compound->getMemberIntType(0);
			H5::StrType string_type = compound->getMemberStrType(1);

			if (string_type.getSize() < v.size()) {
				throw std::runtime_error("Not enough space reserved for string");
			}

			write_number_of_size(ptr, int_type.getSize(), v.size());
			write_string_of_size(ptr, string_type.getSize(), v);
		} else {
			if (datatype.getClass() != H5T_STRING) {
				throw std::runtime_error("Datatype and value do not match");
			}

			H5::StrType* string_type = (H5::StrType*) &datatype;

			// TFK: Check whether this does not leak resources
			if (*string_type == H5::StrType(H5::PredType::C_S1, H5T_VARIABLE)) {
				write(ptr, v);
			} else {
				if (string_type->getSize() < v.size()) {
					throw std::runtime_error("Not enough space reserved for string");
				}
				write_string_of_size(ptr, string_type->getSize(), v);
			}
		}		
	}

	void visit(void*& ptr, H5::DataType& datatype, select_item& v);

	void visit(void*& ptr, H5::DataType& datatype, IfcUtil::IfcBaseClass*& v) {
		
		// This does not seem to help
		// if (datatype.committed()) {
		//	std::cout << datatype.getObjName();
		// }

		if (is_select(datatype)) {
			select_item si(v);
			return visit(ptr, datatype, si);
		}

		pointer_increment_assert _(ptr, datatype.getSize());

		std::pair<int, int> ref = instance_locator_(v);

		if (referenced_) {
			if (datatype.getClass() != H5T_REFERENCE) {
				throw std::runtime_error("Datatype and value do not match");
			}
			hsize_t dims = instance_locator_.instances(v->declaration().type()).size();
			H5::DataSpace space(1, &dims);
			hsize_t coord = ref.second;
			space.selectElements(H5S_SELECT_SET, 1, &coord);
			// TODO: Make path configurable
			file_.reference(ptr, "population/" + instance_locator_.path(ref.first) + "_instances", space);
			advance(ptr, sizeof(hdset_reg_ref_t));
		} else {
			if (datatype.getClass() != H5T_COMPOUND) {
				throw std::runtime_error("Datatype and value do not match");
			}

			if (compound_member_types_as_pair(datatype) != std::make_pair(H5T_INTEGER, H5T_INTEGER)) {
				throw std::runtime_error("Datatype and value do not match");
			}

			H5::CompType* compound = (H5::CompType*) &datatype;
			H5::IntType ds_idx = compound->getMemberIntType(0);
			H5::IntType ds_row = compound->getMemberIntType(1);

			write_number_of_size(ptr, ds_idx.getSize(), ref.first);
			write_number_of_size(ptr, ds_row.getSize(), ref.second);
		}
	}

	template <typename T>
	void visit(void*& ptr, H5::DataType& datatype, std::vector<T>& v) {
		pointer_increment_assert _(ptr, datatype.getSize());
		if (padded_) {
			if (datatype.getClass() != H5T_COMPOUND) {
				throw std::runtime_error("Datatype and value do not match");
			}

			if (compound_member_types_as_pair(datatype) != std::make_pair(H5T_INTEGER, H5T_ARRAY)) {
				throw std::runtime_error("Datatype and value do not match");
			}

			H5::CompType* compound = (H5::CompType*) &datatype;
			H5::IntType size_dt = compound->getMemberIntType(0);
			H5::ArrayType array_dt = compound->getMemberArrayType(1);

			write_number_of_size(ptr, size_dt.getSize(), v.size());
			
			// TFK: Enable multi-dimensional arrays for LISTS of LISTS as well.
			if (array_dt.getArrayNDims() != 1) {
				throw std::runtime_error("Unexpected array dimensions");
			}

			hsize_t n;
			array_dt.getArrayDims(&n);
			H5::DataType elem_dt = array_dt.getSuper();

			for (size_t i = 0; i < n; ++i) {
				if (i < v.size()) {
					// I only get this to work if I make a copy.
					// cannot convert from 'std::_Vb_reference<std::_Wrap_alloc<std::allocator<_Other>>>' to 'const boost::none_t' :(
					T t = v[i];
					visit(ptr, elem_dt, t);
				} else {
					default_value_visitor visitor(elem_dt);
					visitor(ptr);
				}
			}
		} else {
			if (datatype.getClass() != H5T_VLEN) {
				throw std::runtime_error("Datatype and value do not match");
			}

			H5::VarLenType* vlen = (H5::VarLenType*) &datatype;
			H5::DataType elem_dt = vlen->getSuper();

			void* vlen_data = new uint8_t[elem_dt.getSize() * v.size()];
			void* temp = vlen_data;
			for (size_t i = 0; i < v.size(); ++i) {
				T t = v[i];
				visit(temp, elem_dt, t);
			}
			
			write_vlen_t(ptr, v.size(), vlen_data);
		}
	}

};

// template <typename LocatorT>
class write_visit_instance_attribute {
private:
	void*& ptr_;
	write_visit& fn_;
	H5::DataType& dt_;

public:
	typedef void return_type;

	write_visit_instance_attribute(void*& ptr, write_visit& fn, H5::DataType& dt)
		: ptr_(ptr)
		, fn_(fn)
		, dt_(dt)
	{}

	template <typename T>
	void operator()(T& t) {
		fn_.visit(ptr_, dt_, t);
	}
};

static const std::string Entity_Instance_Identifier = "Entity-Instance-Identifier";
static const std::string set_unset_bitmap = "set_unset_bitmap";

template <typename T>
void IfcParse::IfcHdf5File::write_instance(void*& ptr, T& visitor, H5::DataType& datatype, IfcUtil::IfcBaseEntity* v) {
	pointer_increment_assert _(ptr, datatype.getSize());

	if (datatype.getClass() != H5T_COMPOUND) {
		throw std::runtime_error("Datatype and value do not match");
	}

	H5::CompType* compound = (H5::CompType*) &datatype;

	auto attributes = v->declaration().all_attributes();
	// TFK: Creating copies
	// TFK: Do this once for every entity
	std::vector<std::string> attribute_names;
	attribute_names.reserve(attributes.size());

	std::transform(attributes.begin(), attributes.end(),
		std::back_inserter(attribute_names),
		[](const IfcParse::entity::attribute* attr) {
		return attr->name();
	});

	void* set_unset_bitmap_location = 0;
	uint32_t set_unset_bitmap_value = 0;
	size_t set_unset_bitmap_size = 0;

	for (int i = 0; i < compound->getNmembers(); ++i) {
		const std::string name = compound->getMemberName(i);
		H5::DataType attr_type = compound->getMemberDataType(i);

		if (name == Entity_Instance_Identifier) {
			write_number_of_size(ptr, attr_type.getSize(), v->data().id());
			continue;
		} else if (name == set_unset_bitmap) {
			set_unset_bitmap_location = ptr;
			set_unset_bitmap_size = attr_type.getSize();
			advance(ptr, set_unset_bitmap_size);
			continue;
		}

		auto it = std::find(attribute_names.begin(), attribute_names.end(), name);
		if (it == attribute_names.end()) {
			throw std::runtime_error("Unexpected compound member");
		}
		int idx = std::distance(attribute_names.begin(), it);
		
		Argument* attr = v->data().getArgument(idx);
		if (!attr->isNull()) {
			// TODO: This is now index in IFC-attributes list, specify
			set_unset_bitmap_value |= (1 << idx);
		}
		write_visit_instance_attribute/*<typename T::locator_type>*/ attribute_visitor(ptr, visitor, attr_type);
		apply_attribute_visitor(attr, attributes[idx]).apply(attribute_visitor);
	}

	if (set_unset_bitmap_size && set_unset_bitmap_location) {
		write_number_of_size(set_unset_bitmap_location, set_unset_bitmap_size, set_unset_bitmap_value);
	}
}

void write_visit::visit(void*& ptr, H5::DataType& datatype, select_item& v) {
	pointer_increment_assert _(ptr, datatype.getSize());

	IfcUtil::IfcBaseClass* data = v;

	H5::CompType* compound = (H5::CompType*) &datatype;
	const bool data_is_entity = !!data->declaration().as_entity();

	for (int i = 0; i < compound->getNmembers(); ++i) {
		const std::string name = compound->getMemberName(i);
		H5::DataType attr_type = compound->getMemberDataType(i);

		if (name == "type_code") {
			write_number_of_size(ptr, attr_type.getSize(), data->declaration().type());
		} else if (data_is_entity && name == "instance-value") {
			visit(ptr, attr_type, data);
		} else if (type_mapper_.make_select_leaf(&data->declaration(), no_instances).first == name) {
			write_visit_instance_attribute/*<typename T::locator_type>*/ attribute_visitor(ptr, *this, attr_type);
			apply_attribute_visitor(data->data().getArgument(0), 0).apply(attribute_visitor);
		} else {
			default_value_visitor visitor(attr_type);
			visitor(ptr);
		}
	}
}

H5::CompType* create_compound(const std::vector< IfcParse::IfcHdf5File::compound_member >& members) {
	size_t s = 0, o = 0;
	for (auto it = members.begin(); it != members.end(); ++it) {
		s += it->second->getSize();
	}

	H5::CompType* h5_dt = new H5::CompType(s);
	for (auto it = members.begin(); it != members.end(); ++it) {
		h5_dt->insertMember(it->first, o, *it->second);
		o += it->second->getSize();
	}

	return h5_dt;
}

H5::EnumType* create_enumeration(const std::vector<std::string>& items, int offset = 0) {
	size_t numbytes = 0;
	size_t size = items.size();
	while (size != 0) {
		size >>= 8;
		numbytes++;
	}

	int i = offset;
	H5::EnumType* h5_enum = new H5::EnumType(numbytes);
	for (auto it = items.begin(); it != items.end(); ++it, ++i) {
		h5_enum->insert(it->c_str(), &i);
	}

	return h5_enum;
}

void visit_select(const IfcParse::select_type* pt, std::set<const IfcParse::declaration*>& leafs) {
	for (auto it = pt->select_list().begin(); it != pt->select_list().end(); ++it) {
		if ((*it)->as_select_type()) {
			visit_select((*it)->as_select_type(), leafs);
		} else {
			leafs.insert(*it);
		}
	}
}

class UnmetDependencyException : public std::exception {};

std::string type_mapper::flatten_aggregate_name(const IfcParse::parameter_type* at) const {
	if (at->as_aggregation_type()) {
		return std::string("aggregate-of-") + flatten_aggregate_name(at->as_aggregation_type()->type_of_element());
	} else if (at->as_simple_type()) {
		return default_type_names_[at->as_simple_type()->declared_type()];
	} else if (at->as_named_type()) {
		// TODO: Unwind type name
		return at->as_named_type()->declared_type()->name();
	} else {
		throw;
	}
}

type_mapper::type_mapper() {
	default_type_names_[IfcParse::simple_type::logical_type] = "logical";
	default_type_names_[IfcParse::simple_type::boolean_type] = "boolean";
	default_type_names_[IfcParse::simple_type::binary_type] = "binary";
	default_type_names_[IfcParse::simple_type::real_type] = "real";
	default_type_names_[IfcParse::simple_type::number_type] = "number";
	default_type_names_[IfcParse::simple_type::string_type] = "string";
	default_type_names_[IfcParse::simple_type::integer_type] = "integer";

	default_cpp_type_names_[IfcUtil::Argument_BOOL] = "boolean";
	default_cpp_type_names_[IfcUtil::Argument_DOUBLE] = "real";
	default_cpp_type_names_[IfcUtil::Argument_STRING] = "string";
	default_cpp_type_names_[IfcUtil::Argument_INT] = "integer";
}

type_mapper::type_mapper(IfcParse::IfcFile* ifc_file, H5::H5File* hdf5_file, const IfcParse::Hdf5Settings& settings)
	: ifc_file_(ifc_file)
	, hdf5_file_(hdf5_file)
	, settings_(settings)
	, padded_(settings_.profile() == IfcParse::Hdf5Settings::padded || settings_.profile() == IfcParse::Hdf5Settings::padded_referenced)
	, referenced_(settings_.profile() == IfcParse::Hdf5Settings::standard_referenced || settings_.profile() == IfcParse::Hdf5Settings::padded_referenced)
{
	default_type_names_.resize(IfcParse::simple_type::datatype_COUNT);
	default_cpp_type_names_.resize(IfcUtil::Argument_UNKNOWN);

	default_type_names_[IfcParse::simple_type::logical_type] = "logical";
	default_type_names_[IfcParse::simple_type::boolean_type] = "boolean";
	default_type_names_[IfcParse::simple_type::binary_type] = "binary";
	default_type_names_[IfcParse::simple_type::real_type] = "real";
	default_type_names_[IfcParse::simple_type::number_type] = "number";
	default_type_names_[IfcParse::simple_type::string_type] = "string";
	default_type_names_[IfcParse::simple_type::integer_type] = "integer";

	default_cpp_type_names_[IfcUtil::Argument_BOOL] = "boolean";
	default_cpp_type_names_[IfcUtil::Argument_DOUBLE] = "real";
	default_cpp_type_names_[IfcUtil::Argument_STRING] = "string";
	default_cpp_type_names_[IfcUtil::Argument_INT] = "integer";

	if (!ifc_file_ && !hdf5_file_) {
		// This mapper is only used to construct leaf names for select compounds
		return;
	}

	schema_group_ = hdf5_file_->openGroup(ifc_file_->schema()->name() + "_encoding");

	default_types_.resize(IfcParse::simple_type::datatype_COUNT);
	declared_types_.resize(IfcSchema::Type::UNDEFINED);

	if (referenced_) {
		instance_reference_ = commit(new H5::PredType(H5::PredType::STD_REF_DSETREG), "_HDF_INSTANCE_REFERENCE_HANDLE_");
	} else {
		std::vector< IfcParse::IfcHdf5File::compound_member > members;
		members.push_back(std::make_pair(std::string("_HDF5_dataset_index_"), new H5::PredType(H5::PredType::NATIVE_INT16)));
		members.push_back(std::make_pair(std::string("_HDF5_instance_index_"), new H5::PredType(H5::PredType::NATIVE_INT32)));
		instance_reference_ = commit(create_compound(members), "_HDF_INSTANCE_REFERENCE_HANDLE_");
	}

	{
		std::vector<std::string> names;
		names.push_back("BOOLEAN-FALSE");
		names.push_back("BOOLEAN-TRUE");
		default_types_[IfcParse::simple_type::boolean_type] = create_enumeration(names);
	}

	{
		std::vector<std::string> names;
		names.push_back("LOGICAL-UNKNOWN");
		names.push_back("LOGICAL-FALSE");
		names.push_back("LOGICAL-TRUE");
		default_types_[IfcParse::simple_type::logical_type] = create_enumeration(names, -1);
	}

	default_types_[IfcParse::simple_type::binary_type] = &H5::PredType::NATIVE_OPAQUE; // vlen?
	default_types_[IfcParse::simple_type::real_type] = &H5::PredType::NATIVE_DOUBLE;
	default_types_[IfcParse::simple_type::number_type] = &H5::PredType::NATIVE_DOUBLE;
	default_types_[IfcParse::simple_type::string_type] = new H5::StrType(H5::PredType::C_S1, H5T_VARIABLE);
	default_types_[IfcParse::simple_type::integer_type] = &H5::PredType::NATIVE_INT;
}

H5::DataType* type_mapper::commit(H5::DataType* dt, const std::string& name) {
	dt->commit(schema_group_, name);
	return dt;
}

H5::DataType* type_mapper::operator()(const IfcParse::parameter_type* pt, const boost::optional< std::vector<Argument*> >& instances, int dims, const hsize_t* max_length) {
	H5::DataType* h5_dt = nullptr;
	if (pt->as_aggregation_type()) {
		if (padded_ && dims && max_length) {
			// TODO: Check whether these pointers can safely be dereferenced

			// Zero dim arrays are not supported
			hsize_t* max_length_copy = new hsize_t[dims];
			memcpy(max_length_copy, max_length, sizeof(hsize_t) * dims);
			if (max_length_copy[0] == 0) max_length_copy[0] = 1;
			
			// Assume attribute lists are homogeneous and can be directly passed to supertypes
			H5::DataType* element_dt = (*this)(pt->as_aggregation_type()->type_of_element(), instances, dims - 1, max_length + 1);

			if (element_dt == nullptr) {
				// in case of select with all instance refs
				element_dt = instance_reference_;
			}

			std::vector< IfcParse::IfcHdf5File::compound_member > members;
			members.push_back(std::make_pair(std::string("length"), new H5::PredType(H5::PredType::NATIVE_UINT32)));
			members.push_back(std::make_pair(std::string("data"), new H5::ArrayType(*element_dt, 1, max_length_copy)));
			h5_dt = create_compound(members);

			delete[] max_length_copy;
		} else {
			h5_dt = new H5::VarLenType((*this)(pt->as_aggregation_type()->type_of_element()));
		}
	} else if (pt->as_named_type()) {
		if (pt->as_named_type()->declared_type()->as_entity()) {
			return instance_reference_;
			// TFK: Do not copy, we want to retain path to simplify datatype identify checks later on
			// h5_dt = new H5::DataType();
			// h5_dt->copy(*instance_reference_);
		} else {

			if (padded_ && dims && max_length) {
				auto decl = pt->as_named_type()->declared_type();
				while (decl->as_type_declaration()) {
					const IfcParse::parameter_type* leaf_pt = decl->as_type_declaration()->declared_type();
					const IfcParse::named_type* nt = leaf_pt->as_named_type();
					if (nt) {
						decl = nt->declared_type();
					} else {
						break;
					}
				}
				if (
					decl->as_type_declaration() &&
					decl->as_type_declaration()->declared_type()->as_simple_type() &&
					decl->as_type_declaration()->declared_type()->as_simple_type()->declared_type() == IfcParse::simple_type::string_type)
				{
					std::vector< IfcParse::IfcHdf5File::compound_member > members;
					members.push_back(std::make_pair(std::string("length"), new H5::PredType(H5::PredType::NATIVE_UINT32)));
					members.push_back(std::make_pair(std::string("data"), new H5::StrType(H5::PredType::C_S1, (size_t) max_length[0] + 1)));
					h5_dt = create_compound(members);
				} else if (decl->as_type_declaration() && decl->as_type_declaration()->declared_type()->as_aggregation_type()) {
					H5::DataType* element_dt = (*this)(decl->as_type_declaration()->declared_type()->as_aggregation_type()->type_of_element(), instances, dims - 1, max_length + 1);

					if (element_dt == nullptr) {
						// in case of select with all instance refs
						element_dt = instance_reference_;
					}

					std::vector< IfcParse::IfcHdf5File::compound_member > members;
					members.push_back(std::make_pair(std::string("length"), new H5::PredType(H5::PredType::NATIVE_UINT32)));
					members.push_back(std::make_pair(std::string("data"), new H5::ArrayType(*element_dt, 1, max_length)));
					h5_dt = create_compound(members);
				}

			}

			if (!h5_dt) {
				IfcSchema::Type::Enum ty = pt->as_named_type()->declared_type()->type();
				if (!declared_types_[ty]) {
					if (padded_) {
						if (pt->as_named_type()->declared_type()->as_select_type()) {
							// For padded ifc-hdf the select type is based on model population
							return (*this)(pt->as_named_type()->declared_type()->as_select_type(), instances);
						} else if (pt->as_named_type()->declared_type()->as_type_declaration()) {
							return (*this)(pt->as_named_type()->declared_type()->as_type_declaration()->declared_type(), instances);
						} else {
							throw UnmetDependencyException();
						}
					} else {
						throw UnmetDependencyException();
					}
				} else {
					// TFK: Copy, as well be committed under different name?
					h5_dt = new H5::DataType();
					h5_dt->copy(*declared_types_[ty]);
				}
			}
		}
	} else if (pt->as_simple_type()) {
		// TODO: This branch has redundancies with pt->as_named_type
		if (padded_ && dims && max_length && pt->as_simple_type()->declared_type() == IfcParse::simple_type::string_type) {
			std::vector< IfcParse::IfcHdf5File::compound_member > members;
			members.push_back(std::make_pair(std::string("length"), new H5::PredType(H5::PredType::NATIVE_UINT32)));
			members.push_back(std::make_pair(std::string("data"), new H5::StrType(H5::PredType::C_S1, (size_t) max_length[0] + 1)));
			h5_dt = create_compound(members);
		} else {
			// TFK: Copy, as well be committed under different name?
			const H5::DataType* orig = default_types_[pt->as_simple_type()->declared_type()];
			h5_dt = new H5::DataType();
			h5_dt->copy(*orig);
		}
	} else {
		throw UnmetDependencyException();
	}

	return h5_dt;
}

class max_length_visitor {
private:
	std::vector<hsize_t> max_length_;
	void contain(size_t dim, size_t count) {
		if (dim == max_length_.size()) {
			max_length_.push_back(count);
		} else if (count > max_length_[dim]) {
			max_length_[dim] = count;
		}
	}

public:
	typedef void return_type;

	void operator()(const IfcEntityList::ptr& aggregate) {
		contain(0, aggregate->size());
	}

	void operator()(const IfcEntityListList::ptr& aggregate) {
		contain(0, aggregate->size());
		for (auto it = aggregate->begin(); it != aggregate->end(); ++it) {
			contain(1, it->size());
		}
	}

	void operator()(const std::string& str) {
		contain(0, str.size());
	}

	void operator()(const std::vector<std::string>& aggregate) {
		contain(0, aggregate.size());
		for (auto it = aggregate.begin(); it != aggregate.end(); ++it) {
			contain(1, it->size());
		}
	}

	template <typename T>
	void operator()(const std::vector< std::vector<T> >& aggregate) {
		contain(0, aggregate.size());
		for (auto it = aggregate.begin(); it != aggregate.end(); ++it) {
			contain(1, it->size());
		}
	}

	template <typename T>
	void operator()(const std::vector<T>& aggregate) {
		contain(0, aggregate.size());
	}

	template <typename T>
	void operator()(const T&) {
		// Empty on purpose
	}

	operator bool() const {
		return !max_length_.empty();
	}

	int dims() const {
		return (int)max_length_.size();
	}

	const hsize_t* max_length() const {
		return max_length_.data();
	}
};

std::pair<std::string, const H5::DataType*> type_mapper::make_select_leaf(const IfcParse::declaration* decl, const boost::optional< std::vector<Argument*> >& instances) {
	std::string name;
	const H5::DataType* dt = 0;

	const bool should_return_type = ifc_file_ != nullptr && hdf5_file_ != nullptr;

	if (decl) {
		while (decl->as_type_declaration()) {
			const IfcParse::parameter_type* leaf_pt = decl->as_type_declaration()->declared_type();
			const IfcParse::named_type* nt = leaf_pt->as_named_type();
			const IfcParse::simple_type* st = leaf_pt->as_simple_type();
			const IfcParse::aggregation_type* at = leaf_pt->as_aggregation_type();
			if (nt) {
				decl = nt->declared_type();
			} else if (st) {
				name = default_type_names_[st->declared_type()];
				if (padded_ && instances && st->declared_type() == IfcParse::simple_type::string_type) {
					max_length_visitor visitor;
					std::for_each(instances->begin(), instances->end(), [&visitor](Argument* attr) {
						if (attr->type() != IfcUtil::Argument_ENTITY_INSTANCE) {
							throw std::runtime_error("Expected an entity instance or simple type");
						}
						IfcUtil::IfcBaseClass* inst = *attr;
						if (!inst->declaration().as_entity()) {
							Argument* simple_type = inst->data().getArgument(0);
							if (simple_type->type() == IfcUtil::Argument_STRING) {
								// Reference to schema_attr is to resolve enumeration and unknown, it's not necessary here as we explicitely filter on strings.
								apply_attribute_visitor(simple_type, nullptr).apply(visitor);
							}
						}
					});

					if (!visitor) {
						throw std::runtime_error("Unable to determine string width within select type");
					}

					std::vector< IfcParse::IfcHdf5File::compound_member > members;
					members.push_back(std::make_pair(std::string("length"), new H5::PredType(H5::PredType::NATIVE_UINT32)));
					members.push_back(std::make_pair(std::string("data"), new H5::StrType(H5::PredType::C_S1, (size_t)visitor.max_length()[0] + 1)));
					if (should_return_type) dt = create_compound(members);

				} else {
					if (should_return_type) dt = default_types_[st->declared_type()];
				}
				break;
			} else if (at) {
				name = flatten_aggregate_name(at);
				if (should_return_type) dt = (*this)(at);
				break;
			}
		}
	}

	if (!dt && should_return_type) {
		if (decl->as_entity()) {
			name = "instance";
			dt = instance_reference_;
		} else if (decl->as_enumeration_type()) {
			name = decl->name();
			dt = declared_types_[decl->type()];
		} else {
			throw;
		}
	}

	name += "-value";

	return std::make_pair(name, dt);
}

H5::DataType* type_mapper::operator()(const IfcParse::select_type* pt, const boost::optional< std::vector<Argument*> >& instances, int, const hsize_t*) {
	std::set<const IfcParse::declaration*> leafs_schema;
	visit_select(pt, leafs_schema);

	std::set<const IfcParse::declaration*> leafs_model;
	std::set<const IfcParse::declaration*>* leafs = &leafs_schema;

	if (padded_ && instances) {
		std::set<const IfcParse::declaration*> instantiated;
		std::for_each(instances->begin(), instances->end(), [&instantiated](Argument* attr) {
			auto add_to_instantiation = [&instantiated](IfcUtil::IfcBaseClass* inst){
				instantiated.insert(&inst->declaration());
				if (inst->declaration().as_entity()) {
					auto entity = inst->declaration().as_entity();
					while (entity->supertype()) {
						entity = entity->supertype();
						instantiated.insert(entity);
					}
				}
			};
			
			switch (attr->type()) {
			// Should check for null?
			case IfcUtil::Argument_ENTITY_INSTANCE: {
				IfcUtil::IfcBaseClass* inst = *attr;
				add_to_instantiation(inst);
				break;
			}
			case IfcUtil::Argument_AGGREGATE_OF_ENTITY_INSTANCE: {
				IfcEntityList::ptr insts = *attr;
				std::for_each(insts->begin(), insts->end(), add_to_instantiation);
				break;
			}
			default:
				throw std::exception("Unexpected attribute types for datatype width reference");
			}
		});
		std::set_intersection(leafs_schema.begin(), leafs_schema.end(), instantiated.begin(), instantiated.end(), std::inserter(leafs_model, leafs_model.end()));
		leafs = &leafs_model;
	}

	bool all_entity_instance_refs = true;
	for (auto it = leafs->begin(); it != leafs->end(); ++it) {
		if (!(*it)->as_entity()) {
			all_entity_instance_refs = false;
			break;
		}
	}

	if (all_entity_instance_refs) {
		return 0;
	} else {
		std::set<std::string> member_names;
		std::vector<IfcParse::IfcHdf5File::compound_member> h5_attributes;

		h5_attributes.push_back(std::make_pair(std::string("type_code"), &H5::PredType::NATIVE_INT16));

		for (auto it = leafs->begin(); it != leafs->end(); ++it) {
			const IfcParse::declaration* decl = (*it);
			auto leaf_type = make_select_leaf(decl, instances);

			if (member_names.find(leaf_type.first) != member_names.end()) {
				continue;
			}
			member_names.insert(leaf_type.first);

			h5_attributes.push_back(leaf_type);
		}

		return create_compound(h5_attributes);
	}
}

class apply_attribute_visitor_instance_list {
private:
	const IfcEntityList::ptr& instances_;
	int attribute_idx_;
public:
	apply_attribute_visitor_instance_list(const IfcEntityList::ptr& instances, int attribute_idx)
		: instances_(instances)
		, attribute_idx_(attribute_idx)
	{}

	template <typename T>
	typename T::return_type apply(T& t) const {
		for (auto it = instances_->begin(); it != instances_->end(); ++it) {
			apply_attribute_visitor((**it).data().getArgument(attribute_idx_), (**it).declaration().as_entity()->all_attributes()[attribute_idx_]).apply(t);
		}
	}
};

class all_null_visitor {
private:
	bool all_null_;
public:
	typedef void return_type;

	all_null_visitor()
		: all_null_(true)
	{}

	void operator()(const boost::none_t&) {
		// Empty on purpose
	}

	template <typename T>
	void operator()(const T&) {
		all_null_ = false;
	}

	bool all_null() const {
		return all_null_;
	}
};

H5::CompType* type_mapper::operator()(const IfcParse::entity* e, const boost::optional< std::vector<Argument*> >&, int, const hsize_t*) {

	// List of entity instances of this type, no subtypes
	IfcEntityList::ptr incl_subtypes = ifc_file_->entitiesByType(e->name());
	IfcEntityList::ptr instances(new IfcEntityList);
	if (incl_subtypes) {
		for (auto it = incl_subtypes->begin(); it != incl_subtypes->end(); ++it) {
			if ((**it).declaration().type() == e->type()) {
				instances->push(*it);
			}
		}
	}

	if (instances->size() == 0) {
		return 0;
	}

	std::vector<const IfcParse::entity::attribute*> attributes = e->all_attributes();
	std::vector<const IfcParse::entity::inverse_attribute*> inverse_attributes;
	if (settings_.instantiate_inverse()) {
		inverse_attributes = e->all_inverse_attributes();
	}

	const std::vector<bool>& attributes_derived_in_subtype = e->derived();

	bool has_optional_attributes = false;
	const size_t num_extra = has_optional_attributes ? 2 : 1;
	size_t num_all_null = 0;
	std::vector<bool> attributes_all_null(attributes.size(), false);

	auto jt = attributes_derived_in_subtype.begin();
	for (auto it = attributes.begin(); it != attributes.end(); ++it, ++jt) {
		if (*jt) {
			continue;
		}
		if ((**it).optional()) {
			const int idx = std::distance(attributes.begin(), it);

			has_optional_attributes = true;

			apply_attribute_visitor_instance_list dispatch(instances, idx);
			all_null_visitor visitor;
			dispatch.apply(visitor);
			if (visitor.all_null()) {
				num_all_null += 1;
				attributes_all_null[idx] = true;
				// attributes_omitted.insert(std::make_pair(e->name(), idx));
			}
		}
	}

	/*
	if (has_optional_attributes) {
		entities_with_optional_attrs.insert(e->name());
	}
	*/

	std::vector< IfcParse::IfcHdf5File::compound_member > h5_attributes;
	h5_attributes.reserve(attributes.size() + inverse_attributes.size() + num_extra);

	if (has_optional_attributes) {
		h5_attributes.push_back(std::make_pair(std::string("set_unset_bitmap"), &H5::PredType::NATIVE_INT16));
	}
	h5_attributes.push_back(std::make_pair(std::string("Entity-Instance-Identifier"), &H5::PredType::NATIVE_INT32));

	jt = attributes_derived_in_subtype.begin();
	for (auto it = attributes.begin(); it != attributes.end(); ++it, ++jt) {
		if (*jt) {
			continue;
		}

		const int idx = std::distance(attributes.begin(), it);

		if (padded_ && attributes_all_null[idx]) {
			continue;
		}

		// TFK: Note the scope of max_length_visitor
		max_length_visitor visitor;
		const hsize_t* max_length = nullptr;
		int dims = 0;

		if (padded_) {
			apply_attribute_visitor_instance_list dispatch(instances, idx);
			dispatch.apply(visitor);
			if (visitor) {
				max_length = visitor.max_length();
				dims = visitor.dims();
			}
		}

		boost::optional< std::vector<Argument*> > attribute_select_instances;
		
		const bool is_select = (*it)->type_of_attribute()->as_named_type() && (*it)->type_of_attribute()->as_named_type()->declared_type()->as_select_type();
		const bool is_select_aggregate = (*it)->type_of_attribute()->as_aggregation_type() &&
			(*it)->type_of_attribute()->as_aggregation_type()->type_of_element()->as_named_type() &&
			(*it)->type_of_attribute()->as_aggregation_type()->type_of_element()->as_named_type()->declared_type()->as_select_type();

		if (padded_ && (is_select || is_select_aggregate)) {
			attribute_select_instances.emplace();
			std::for_each(instances->begin(), instances->end(), [idx, &attribute_select_instances](IfcUtil::IfcBaseClass* inst) {
				Argument* attribute_value = inst->data().getArgument(idx);
				attribute_select_instances->push_back(attribute_value);
			});
		}

		const std::string& name = (*it)->name();
		if (name == "FillStyles") {
			std::cerr << 1;
		}
		const bool is_optional = (*it)->optional();
		const H5::DataType* type;
		const std::string qualified_attr_name = e->name() + "." + name;
		if (std::binary_search(settings_.ref_attributes().begin(), settings_.ref_attributes().end(), qualified_attr_name)) {
			type = new H5::PredType(H5::PredType::STD_REF_OBJ);
		// } else if (overridden_types.find(qualified_attr_name) != overridden_types.end()) {
		// 	type = overridden_types.find(qualified_attr_name)->second;
		// } else if (overridden_types.find("*." + name) != overridden_types.end()) {
		// 	type = overridden_types.find("*." + name)->second;
		} else {
			type = (*this)((*it)->type_of_attribute(), attribute_select_instances, dims, max_length);
			if (type == nullptr) {
				// This is for select types. Should this be handled elsewhere?
				type = instance_reference_;
			}
		}

		// TODO: Re-evaluate
		// if (false && visitor && visitor.max_length()[0] == 0) {
		//	attributes_omitted.insert(std::make_pair(e->name(), idx));
		// } else {
			h5_attributes.push_back(std::make_pair(name, type));
		// }
	}

	if (settings_.instantiate_inverse()) {
		for (auto it = inverse_attributes.begin(); it != inverse_attributes.end(); ++it) {
			const std::string& name = (*it)->name();
			// H5::DataType* ir_copy = new H5::DataType();
			// ir_copy->copy(*instance_reference_);
			const H5::DataType* type = new H5::VarLenType(instance_reference_);
			h5_attributes.push_back(std::make_pair(name, type));
		}
	}

	return create_compound(h5_attributes);
}

H5::EnumType* type_mapper::operator()(const IfcParse::enumeration_type* en, const boost::optional< std::vector<Argument*> >&, int, const hsize_t*) {
	return create_enumeration(en->enumeration_items());
}

void type_mapper::operator()() {
	const IfcParse::schema_definition& schema = *ifc_file_->schema();

	for (auto it = schema.enumeration_types().begin(); it != schema.enumeration_types().end(); ++it) {
		declared_types_[(*it)->type()] = commit((*this)(*it), (*it)->name());
	}

	if (!padded_) {
		// For padded ifc-hdf files the width for declared types will depend on the data in the model.
		// Therefore there is no point in having e.g. an IfcLabel type as part of the serialization.

		const std::vector<const IfcParse::type_declaration*>& ts = schema.type_declarations();
		std::set<const IfcParse::type_declaration*> processed;
		while (processed.size() < ts.size()) {
			for (auto it = ts.begin(); it != ts.end(); ++it) {
				auto pt = (*it)->declared_type();
				const std::string& name = (*it)->name();
				if (processed.find(*it) != processed.end()) continue;
				try {
					declared_types_[(*it)->type()] = commit((*this)(pt), name);
					processed.insert(*it);
				} catch (const UnmetDependencyException&) {}
			}
		}

		for (auto it = schema.select_types().begin(); it != schema.select_types().end(); ++it) {
			H5::DataType* dt = (*this)(*it);
			if (dt) {
				declared_types_[(*it)->type()] = commit(dt, (*it)->name());
			} else {
				declared_types_[(*it)->type()] = instance_reference_;
			}
		}
	}

	for (auto it = schema.entities().begin(); it != schema.entities().end(); ++it) {
		auto dt = (*this)(*it);
		if (dt) {
			declared_types_[(*it)->type()] = commit(dt, (*it)->name());
		}
	}
}

void visit(void* buffer, H5::DataType* dt) {
	if (dt->getClass() == H5T_COMPOUND) {
		H5::CompType* ct = (H5::CompType*) dt;
		for (int i = 0; i < ct->getNmembers(); ++i) {
			std::cerr << ct->getMemberName(i) << " ";
			size_t offs = ct->getMemberOffset(i);
			H5::DataType dt2 = ct->getMemberDataType(i);
			visit((uint8_t*)buffer + offs, &dt2);
			dt2.close();
		}
	} else if (dt->getClass() == H5T_VLEN) {
		hvl_t* ht = (hvl_t*)buffer;
		H5::VarLenType* vt = (H5::VarLenType*) dt;
		H5::DataType dt2 = vt->getSuper();
		for (size_t i = 0; i < ht->len; ++i) {
			std::cerr << i << " ";
			visit((uint8_t*)ht->p + i * vt->getSize(), &dt2);
		}
		dt2.close();
	} else if (dt->getClass() == H5T_STRING) {
		char* c = *(char**)buffer;
		std::cerr << "'" << c << "'" << " ";
	}
}

size_t get_alignment() {
	return 0;
}

H5::EnumType* map_enumeration(const IfcParse::enumeration_type* en) {
	return create_enumeration(en->enumeration_items());
}

H5::DataType* IfcParse::IfcHdf5File::commit(H5::DataType* dt, const std::string& name) {
	dt->commit(schema_group, name);
	return dt;
}



void create_attribute(H5::H5Location& loc, const std::string& name, const std::vector<std::string>& v) {
	const hsize_t dim = v.size();

	H5::DataSpace attr_space(1, &dim);
	H5::StrType attr_type(H5::PredType::C_S1, H5T_VARIABLE);
	H5::Attribute attr = loc.createAttribute(name, attr_type, attr_space);

	char **attr_data, **ptr;
	attr_data = ptr = new char*[dim];
	std::for_each(v.cbegin(), v.cend(), [&ptr](const std::string& s) {
		*ptr = new char[s.size() + 1];
		strcpy(*(ptr++), s.c_str());
	});

	attr.write(attr_type, attr_data);

	attr.close();
	attr_type.close();
	attr_space.close();

	std::for_each(attr_data, ptr, [](char* c) {
		delete[] c;
	});
	delete[] attr_data;
}

void create_attribute(H5::H5Location& loc, const std::string& name, const std::string& v) {
	hsize_t dim = 1;

	H5::DataSpace attr_space(1, &dim);
	H5::StrType attr_type(H5::PredType::C_S1, v.size() + 1);
	H5::Attribute attr = loc.createAttribute(name, attr_type, attr_space);

	attr.write(attr_type, v);

	attr.close();
	attr_type.close();
	attr_space.close();
}

class read_attribute {
private:
	H5::Attribute attribute_;
	H5::DataType attr_type_;
public:
	read_attribute(const H5::H5Location& loc, const std::string& name) {
		attribute_ = loc.openAttribute(name);
		attr_type_ = attribute_.getDataType();
	}

	operator std::string() {
		std::string str;
		attribute_.read(attr_type_, str);
		return str;
	}

	operator std::vector<std::string>() {
		H5::DataSpace space = attribute_.getSpace();
		hsize_t dim;
		space.getSimpleExtentDims(&dim);
		std::vector<std::string> vec(dim);
		char** buffer = new char*[dim];
		attribute_.read(attr_type_, buffer);
		for (size_t i = 0; i < dim; ++i) {
			vec[i].assign(buffer[i]);
		}
		delete[] buffer;
		return vec;
	}

	~read_attribute() {
		attr_type_.close();
		attribute_.close();
	}
};

void IfcParse::IfcHdf5File::write_schema(const IfcParse::schema_definition& schema, IfcParse::IfcFile& ifc_file) {
	// From h5ex_g_compact.c
	// Compact groups?
	H5::FileAccPropList* plist = new H5::FileAccPropList();
	plist->setLibverBounds(H5F_LIBVER_LATEST, H5F_LIBVER_LATEST);

	// H5::FileCreatPropList* plist2 = new H5::FileCreatPropList();
	// H5Pset_file_space(plist2->getId(), H5F_FILE_SPACE_VFD, (hsize_t)0);
	
	file = new H5::H5File(name_, H5F_ACC_TRUNC, H5::FileCreatPropList::DEFAULT, *plist);

	schema_group = file->createGroup(schema.name() + "_encoding");
	population_group = file->createGroup("population");
	create_attribute(schema_group, "iso_10303_26_schema", IfcSchema::Identifier);
	
	// init_default_types();

	mapper_ = new type_mapper(&ifc_file, file, settings_);
	(*(type_mapper*)mapper_)();
};

std::pair<size_t, size_t> IfcParse::IfcHdf5File::make_instance_reference(const IfcUtil::IfcBaseClass* instance) const {
#ifdef SORT_ON_NAME
	const std::vector<uint32_t>& es = sorted_entities.find(instance->declaration().type())->second;
#else
	const std::vector<IfcUtil::IfcBaseClass*>& es = sorted_entities.find(instance->declaration().type())->second;
#endif
	
	auto dataset_id = std::lower_bound(dataset_names.begin(), dataset_names.end(), instance->declaration().type());
#ifdef SORT_ON_NAME
	auto instance_id = std::lower_bound(es.begin(), es.end(), instance->data().id());
#else
	auto instance_id = std::lower_bound(es.begin(), es.end(), instance);
#endif
	
	size_t dataset_offset = std::distance(dataset_names.begin(), dataset_id);
	size_t instance_offset = std::distance(es.begin(), instance_id);

	return std::make_pair(dataset_offset, instance_offset);
}

H5::DataSet IfcParse::IfcHdf5File::create_dataset(const std::string& path, H5::DataType datatype, int rank, hsize_t* dimensions) {
	if (rank != 1) {
		throw std::runtime_error("Expected rank 1");
	}

	const size_t datatype_size = datatype.getSize();	

	hsize_t chunk;
	if (settings_.chunk_size() > 0 && settings_.chunk_size() < dimensions[0]) {
		chunk = static_cast<hsize_t>(settings_.chunk_size());
	} else {
		chunk = dimensions[0];
	}

	const H5::DSetCreatPropList* plist;
	// H5O_MESG_MAX_SIZE = 65536
	const bool compact = dimensions[0] * datatype_size < (1 << 15);
	if (settings_.compress() && !compact) {
		H5::DSetCreatPropList* plist_ = new H5::DSetCreatPropList;
		plist_->setChunk(1, &chunk);
		// D'oh. Order is significant, according to h5ex_d_shuffle.c
		plist_->setShuffle();
		plist_->setDeflate(9);
		plist = plist_;
	} else if (compact) {
		H5::DSetCreatPropList* plist_ = new H5::DSetCreatPropList;
		// Set compact according to h5ex_d_compact.c
		plist_->setLayout(H5D_COMPACT);
		plist = plist_;
	} else if (chunk != dimensions[0]) {
		H5::DSetCreatPropList* plist_ = new H5::DSetCreatPropList;
		plist_->setChunk(1, &chunk);
		plist = plist_;
	} else {
		plist = &H5::DSetCreatPropList::DEFAULT;
	}

	H5::DataSpace space(1, dimensions);
	H5::DataSet ds = population_group.createDataSet(path, datatype, space, *plist);

	if (plist != &H5::DSetCreatPropList::DEFAULT) {
		// ->close() doesn't work due to const, hack hack hack
		H5Pclose(plist->getId());
	}

	return ds;
}

void IfcParse::IfcHdf5File::write_header(H5::Group& group, IfcSpfHeader& header) {
	create_attribute(group, "iso_10303_26_data", IfcSchema::Identifier);
	
	create_attribute(group, "iso_10303_26_description", header.file_description().description());
	create_attribute(group, "iso_10303_26_implementation_level", header.file_description().implementation_level());
	
	create_attribute(group, "iso_10303_26_name", header.file_name().name());
	create_attribute(group, "iso_10303_26_time_stamp", header.file_name().time_stamp());
	create_attribute(group, "iso_10303_26_author", header.file_name().author());
	create_attribute(group, "iso_10303_26_organization", header.file_name().organization());
	create_attribute(group, "iso_10303_26_preprocessor_version", header.file_name().preprocessor_version());
	create_attribute(group, "iso_10303_26_originating_system", header.file_name().originating_system());
	create_attribute(group, "iso_10303_26_authorization", header.file_name().authorization());
}

void IfcParse::IfcHdf5File::write_population(IfcFile&) {
	const bool padded = settings_.profile() == IfcParse::Hdf5Settings::padded || settings_.profile() == IfcParse::Hdf5Settings::padded_referenced;
	const bool referenced = settings_.profile() == IfcParse::Hdf5Settings::standard_referenced || settings_.profile() == IfcParse::Hdf5Settings::padded_referenced;
	
	sorted_instance_locator locator(*file, this->ifcfile_, referenced);

	write_header(population_group, this->ifcfile_.header());

	dataset_names.assign(locator.begin(), locator.end());
	std::vector<std::string> dataset_names_string; dataset_names_string.reserve(dataset_names.size());
	std::transform(dataset_names.begin(), dataset_names.end(), std::back_inserter(dataset_names_string), [](IfcSchema::Type::Enum v) {
		return IfcSchema::Type::ToString(v);
	});
	create_attribute(population_group, "iso_10303_26_data_set_names", dataset_names_string);

	write_visit/*<sorted_instance_locator>*/ visitor(*this->file, padded, referenced, locator, *((type_mapper*)mapper_));

	if (referenced) {
		for (auto dsn_it = dataset_names.begin(); dsn_it != dataset_names.end(); ++dsn_it) {
			const std::string current_entity_name = IfcSchema::Type::ToString(*dsn_it);
			const std::string dataset_path = current_entity_name + "_instances";

			hsize_t num_instances = locator.instances(*dsn_it).size();
			H5::DataType entity_datatype = schema_group.openDataType(current_entity_name);

			create_dataset(dataset_path, entity_datatype, 1, &num_instances).close();
			entity_datatype.close();
		}
	}

	for (auto dsn_it = dataset_names.begin(); dsn_it != dataset_names.end(); ++dsn_it) {
		
		const std::string current_entity_name = IfcSchema::Type::ToString(*dsn_it);
		const std::string dataset_path = current_entity_name + "_instances";
		const std::vector<IfcUtil::IfcBaseEntity*>& instances = locator.instances(*dsn_it);
		hsize_t num_instances = instances.size();

		std::cerr << current_entity_name << std::endl;

		H5::DataType entity_datatype = schema_group.openDataType(current_entity_name);
		H5::DataSet dataset;
		
		if (referenced) {
			dataset = population_group.openDataSet(dataset_path);
		} else {
			dataset = create_dataset(dataset_path, entity_datatype, 1, &num_instances);
		}

		size_t dataset_size = entity_datatype.getSize() * static_cast<size_t>(num_instances);
		void* data = allocator.allocate(dataset_size);
		void* ptr = data;
		std::cerr << dataset_size << std::endl;

		for (auto inst_it = instances.begin(); inst_it != instances.end(); ++inst_it) {
			write_instance(ptr, visitor, entity_datatype, (IfcUtil::IfcBaseEntity*)*inst_it);
		}

		dataset.write(data, entity_datatype);
		H5::DataSpace space(1, &num_instances);
		H5Dvlen_reclaim(entity_datatype.getId(), space.getId(), H5P_DEFAULT, data);

		dataset.close();
		space.close();

		// allocator.free();
		delete[] data;
	}
}

bool is_instance_ref(H5::DataType* dt) {
	if (dt->getClass() != H5T_COMPOUND) return false;
	H5::CompType* ct = (H5::CompType*) dt;
	return ct->getMemberName(0) == "_HDF5_dataset_index_";
}

enum padded_datatype_type {
	padded_string,
	padded_array,
	not_padded
};

padded_datatype_type is_padded_datatype(H5::DataType* dt) {
	if (dt->getClass() != H5T_COMPOUND) {
		return not_padded;
	}
	
	H5::CompType* ct = (H5::CompType*) dt;
	
	if (ct->getNmembers() != 2) {
		return not_padded;
	}
	if (ct->getMemberName(0) != "length" || ct->getMemberName(1) != "data") {
		return not_padded;
	}

	if (ct->getMemberClass(1) == H5T_STRING) {
		return padded_string;
	} else {
		return padded_array;
	}
}

template <typename T>
T read_(void* buffer, H5::DataType* dt) {
	(void*)dt;
	T t;
	memcpy(&t, buffer, sizeof(T));
	return t;
}

template <typename T>
T read(void* buffer, H5::DataType* dt) {
	return read_<T>(buffer, dt);
}

template <>
int read(void* buffer, H5::DataType* dt) {
	// TODO: Check for overflow
	// TODO: Check signed
	int i;
	switch (dt->getSize()) {
	case 1: i = (int) read_<int_of_size<1>::type>(buffer, dt); break;
	case 2: i = (int) read_<int_of_size<2>::type>(buffer, dt); break;
	case 4: i = (int) read_<int_of_size<4>::type>(buffer, dt); break;
	case 8: i = (int) read_<int_of_size<8>::type>(buffer, dt); break;
	default: throw std::runtime_error("Unexpected integer width");
	}
	return i;
}

template <>
double read(void* buffer, H5::DataType* dt) {
	double d;
	switch (dt->getSize()) {
	case 4: d = read_<float_of_size<4>::type>(buffer, dt); break;
	case 8: d = read_<float_of_size<8>::type>(buffer, dt); break;
	default: throw std::runtime_error("Unexpected float width");
	}
	return d;
}

static const std::bitset<64> many_trues(std::numeric_limits<unsigned long>::max());
static const std::string no_name = "NO_NAME";

class instance_resolver {
private:
	H5::H5File& file_;
	H5::Group& population_group_;
	std::vector<std::string> dataset_names_;

public:
	instance_resolver(H5::H5File& file, H5::Group& population_group)
		: file_(file)
		, population_group_(population_group)
	{
		dataset_names_ = read_attribute(population_group_, "iso_10303_26_data_set_names");
	}

	size_t instance_name(void* data, H5::CompType* dt) {
		hsize_t pair[2];
		for (int i = 0; i < 2; ++i) {
			H5::DataType mt = dt->getMemberDataType(i);
			size_t offset = dt->getMemberOffset(i);
			pair[i] = read<int>((uint8_t*)data + offset, &mt);
		}

		const std::string& nm = dataset_names_[pair[0]];

		H5::DataSet dataset = file_.openDataSet("population/" + nm + "_instances");
		H5::CompType datatype = dataset.getCompType();
		H5::DataSpace dataspace = dataset.getSpace();
		if (dataspace.getSimpleExtentNdims() != 1) {
			throw std::runtime_error("Expected one-dimensional data");
		}
		dataspace.selectElements(H5S_SELECT_SET, 1, pair + 1);
		const hsize_t one = 1;
		H5::DataSpace memspace(1, &one);

		uint8_t* buffer = new uint8_t[datatype.getSize()];
		dataset.read(buffer, datatype, memspace, dataspace);

		int idx = datatype.getMemberIndex("Entity-Instance-Identifier");
		size_t inst_name_offset = datatype.getMemberOffset(idx);
		H5::DataType member_type = datatype.getMemberDataType(idx);

		size_t inst_name = read<int>((uint8_t*)buffer + inst_name_offset, &member_type);
		
		member_type.close();
		memspace.close();
		datatype.close();
		dataspace.close();
		dataset.close();

		delete[] buffer;

		return inst_name;
	}

	H5::H5File& file() { return file_; }
};

void visit(instance_resolver& resolver, std::ostream& output, void* buffer, H5::DataType* dt, const std::string& name = no_name, const std::bitset<64>& bitmask = many_trues, int padded_length=-1) {
	if (dt->getClass() == H5T_COMPOUND) {

		// std::vector<IfcParse::entity::attribute*> schema_attributes;
		std::vector<bool> derived;
		std::vector<bool>::const_iterator derived_it = derived.begin();
		std::vector<std::string> attribute_names;
		std::vector<std::string>::const_iterator attribute_name_it = attribute_names.begin();
		if (&name != &no_name) {
			auto entity = get_schema().declaration_by_name(IfcSchema::Type::FromString(boost::to_upper_copy(name)))->as_entity();
			auto attributes = entity->all_attributes();
			std::transform(attributes.begin(), attributes.end(), std::back_inserter(attribute_names), [](const IfcParse::entity::attribute* attr) {
				return attr->name();
			});
			derived = entity->derived();
			derived_it = derived.begin();
			attribute_name_it = attribute_names.begin();
		}

		H5::CompType* ct = (H5::CompType*) dt;
		const auto is_ref = is_instance_ref(dt);
		const auto is_padded = is_padded_datatype(dt);

		if (is_ref) {
			output << "#" << resolver.instance_name(buffer, ct);
		} else if (is_padded != not_padded) {
			const size_t offs = ct->getMemberOffset(1);
			void* member_ptr = (uint8_t*)buffer + offs;
			int N = read<int>(buffer, &ct->getMemberDataType(0));
			H5::DataType member_type = ct->getMemberDataType(1);
			
			visit(resolver, output, member_ptr, &member_type, name, many_trues, N);
		} else {
			int ifc_idx = 0;
			const std::bitset<64>* mask = &bitmask;
			std::string select_valuation;
			bool is_instance = false;
			bool is_select = false;
			bool is_selected_simple_type = false;
			bool select_valuation_encountered = false;

			for (int i = 0; i < ct->getNmembers(); ++i) {
				const std::string member_name = ct->getMemberName(i);
				
				const size_t offs = ct->getMemberOffset(i);
				void* member_ptr = (uint8_t*)buffer + offs;
				H5::DataType member_type = ct->getMemberDataType(i);

				if (member_name == "set_unset_bitmap") {
					mask = new std::bitset<64>(read<int>(member_ptr, &member_type));
				} else if (member_name == "Entity-Instance-Identifier") {
					is_instance = true;
					output << "#" << read<int>(member_ptr, &member_type) << "=" << boost::to_upper_copy(name) << "(";
				} else if (member_name == "type_code") {
					is_select = true;
					IfcSchema::Type::Enum ty = (IfcSchema::Type::Enum) read<int>(member_ptr, &member_type);
					auto decl = get_schema().declaration_by_name(ty);
					if (decl->as_entity()) {
						select_valuation = "instance-value";
					} else {
						std::string type_string = IfcSchema::Type::ToString(ty);
						boost::to_upper(type_string);
						output << type_string << "(";
						IfcParse::Hdf5Settings settings;
						select_valuation = type_mapper(nullptr, nullptr, settings).make_select_leaf(decl, no_instances).first;
						is_selected_simple_type = true;
					}
				} else {
					if (is_select) {
						if (member_name == select_valuation) {
							visit(resolver, output, member_ptr, &member_type, name, many_trues);
							select_valuation_encountered = true;
						}
					} else {
						while (member_name != *attribute_name_it) {
							if (ifc_idx) {
								output << ",";
							}
							attribute_name_it++;
							output << ((*derived_it) ? "*" : "$");
							derived_it++;
							ifc_idx++;
						}
						if (ifc_idx) {
							output << ",";
						}
						if ((*mask)[ifc_idx]) {
							visit(resolver, output, member_ptr, &member_type, name, many_trues);
						} else {
							output << "$";
						}
						ifc_idx++;
						attribute_name_it++;
						derived_it++;
					}
				}
				member_type.close();
			}

			if (!is_select) {
				for (; attribute_name_it != attribute_names.end(); ++attribute_name_it, ++derived_it, ++ifc_idx) {
					if (ifc_idx) {
						output << ",";
					}
					output << ((*derived_it) ? "*" : "$");
				}
			}

			if (is_select && !select_valuation_encountered) {
				throw std::runtime_error("Select valuation not encountered in compound");
			}

			if (mask != &many_trues) {
				delete mask;
			}

			if (is_selected_simple_type || is_instance) {
				output << ")";
				if (is_instance) {
					output << ";" << std::endl;
				}
			}
		}
	} else if (dt->getClass() == H5T_VLEN) {
		hvl_t* ht = (hvl_t*)buffer;
		H5::VarLenType* vt = (H5::VarLenType*) dt;
		H5::DataType dt2 = vt->getSuper();
		output << "(";
		for (size_t i = 0; i < ht->len; ++i) {
			if (i) {
				output << ",";
			}
			visit(resolver, output, (uint8_t*)ht->p + i * dt2.getSize(), &dt2);
		}
		output << ")";
		dt2.close();
	} else if (dt->getClass() == H5T_STRING) {
		const bool is_varlen = H5Tis_variable_str(dt->getId()) > 0;
		if (is_varlen) {
			const char* c = read<const char*>(buffer, dt);
			output << "'" << c << "'";
		} else {
			output << "'" << ((char*)buffer) << "'";
		}
	} else if (dt->getClass() == H5T_INTEGER) {
		output << read<int>(buffer, dt);
	} else if (dt->getClass() == H5T_FLOAT) {
		output << IfcWrite::format(read<double>(buffer, dt));
	} else if (dt->getClass() == H5T_ENUM) {
		char enum_value[255];
		H5Tenum_nameof(dt->getId(), buffer, enum_value, 255);
		const char BOOLEAN[] = "BOOLEAN-";
		const char LOGICAL[] = "LOGICAL-";
		if (strstr(enum_value, BOOLEAN) || strstr(enum_value, LOGICAL)) {
			// Hack to convert BOOLEAN-TRUE et al to T
			// Boolean and logical are of the same length coincedentally
			enum_value[0] = enum_value[strlen(BOOLEAN)];
			enum_value[1] = 0;
		} 
		output << "." << enum_value << ".";
	} else if (dt->getClass() == H5T_ARRAY) {
		H5::ArrayType* at = (H5::ArrayType*) dt;
		H5::DataType dt2 = at->getSuper();
		int N = padded_length >= 0 ? padded_length : (int) (at->getSize() / dt2.getSize());
		output << "(";
		for (size_t i = 0; i < N; ++i) {
			if (i) {
				output << ",";
			}
			visit(resolver, output, (uint8_t*)buffer + i * dt2.getSize(), &dt2);
		}
		output << ")";
		dt2.close();
	} else if (dt->getClass() == H5T_REFERENCE) {
		hid_t setid = H5Rdereference(resolver.file().getId(), H5R_DATASET_REGION, buffer);
		hid_t spaceid = H5Rget_region(resolver.file().getId(), H5R_DATASET_REGION, buffer);
		
		H5::DataSet dset(setid);
		H5::DataSpace dspace(spaceid);
		H5::CompType dtype = dset.getCompType();
		
		if (dspace.getSelectNpoints() != 1) {
			throw std::runtime_error("Unexpected number of points selected");
		}
		
		hsize_t selected;
		dspace.getSelectElemPointlist(0, 1, &selected);

		hsize_t dims = 1;
		H5::DataSpace memspace(1, &dims);
		
		uint8_t* buffer = new uint8_t[dtype.getSize()];
		dset.read(buffer, dtype, memspace, dspace);

		const int i = dtype.getMemberIndex(Entity_Instance_Identifier);
		const size_t offs = dtype.getMemberOffset(i);
		H5::DataType instidt = dtype.getMemberDataType(i);

		const int name = read<int>(buffer + offs, &instidt);
		output << "#" << name;

		instidt.close();
		dtype.close();
		dspace.close();
		memspace.close();
		dset.close();

		delete[] buffer;
	} else {
		throw std::runtime_error("Unexpected datatype encountered");
	}
}

struct hdf5_output_info {
	H5::H5File& file;
	std::ostream& output;
};

herr_t iterate(hid_t id, const char *name_, const H5O_info_t *object_info, void *op_data) {
	H5::H5File& f = ((hdf5_output_info*)op_data)->file;
	std::ostream& output = ((hdf5_output_info*)op_data)->output;

	if (object_info->type == H5O_TYPE_DATASET) {
		H5::Group population_group(id);
		instance_resolver resolver(f, population_group);
		
		std::string name = name_;

		H5::DataSet dataset = population_group.openDataSet(name);
		H5::DataType datatype = dataset.getDataType();
		H5::DataSpace dataspace = dataset.getSpace();
		if (dataspace.getSimpleExtentNdims() != 1) {
			throw std::runtime_error("Expected one-dimensional data");
		}
		hsize_t dim;
		dataspace.getSimpleExtentDims(&dim, NULL);
		uint8_t* buffer, *ptr;
		buffer = ptr = new uint8_t[(size_t) dim * datatype.getSize()];
		dataset.read(buffer, datatype);

		auto slash = name.find_last_of('/');
		if (slash != std::string::npos) {
			name = name.substr(slash + 1);
		}
		auto under = name.find('_');
		if (under != std::string::npos) {
			name = name.substr(0, under);
		}

		while (dim--) {
			visit(resolver, output, ptr, &datatype, name);
			ptr += datatype.getSize();
		}

		datatype.close();
		dataspace.close();
		dataset.close();
		population_group.close();

		delete[] buffer;
	}
	return 0;
}

class file_structure {
public:
	typedef std::map<std::string, H5::Group> schema_mapping_t;
	typedef std::vector< std::pair<H5::Group, typename schema_mapping_t::const_iterator> > populations_t;
	typedef populations_t::const_iterator const_iterator;
	
private:
	schema_mapping_t schemas_;
	populations_t populations_;
	H5::H5File& file_;
	bool finished_schema_;

public:
	file_structure(H5::H5File& file)
		: file_(file)
		, finished_schema_(false)
	{}

	void process_group(H5::Group& g) {
		if (finished_schema_) {
			if (g.attrExists("iso_10303_26_data")) {
				const std::string name = read_attribute(g, "iso_10303_26_data");
				if (schemas_.find(name) != schemas_.end()) {
					populations_.push_back(std::make_pair(g, schemas_.find(name)));
				}
			}
		} else {
			if (g.attrExists("iso_10303_26_schema")) {
				const std::string name = read_attribute(g, "iso_10303_26_schema");
				schemas_.insert(std::make_pair(name, g));
			}
		}
	}

	void finished_schema() { finished_schema_ = true; }

	const_iterator begin() const { return populations_.begin(); }
	const_iterator end() const { return populations_.end(); }
	H5::H5File& file() const { return file_; }
};

herr_t find_groups(hid_t, const char *name, const H5O_info_t *object_info, void *op_data) {
	file_structure& fs = *(file_structure*)op_data;
	if (object_info->type == H5O_TYPE_GROUP) {
		H5::Group g = fs.file().openGroup(name);
		fs.process_group(g);
		g.close();
	}
	return 0;
}

void write_spf_header(const H5::Group& group, std::ostream& out) {
	IfcParse::IfcSpfHeader spf_header;
	
	spf_header.file_schema().schema_identifiers(
		std::vector<std::string>{read_attribute(group, "iso_10303_26_data")}
	);

	spf_header.file_description().description(read_attribute(group, "iso_10303_26_description"));
	spf_header.file_description().implementation_level(read_attribute(group, "iso_10303_26_implementation_level"));

	spf_header.file_name().name(read_attribute(group, "iso_10303_26_name"));
	spf_header.file_name().time_stamp(read_attribute(group, "iso_10303_26_time_stamp"));
	spf_header.file_name().author(read_attribute(group, "iso_10303_26_author"));
	spf_header.file_name().organization(read_attribute(group, "iso_10303_26_organization"));
	spf_header.file_name().preprocessor_version(read_attribute(group, "iso_10303_26_preprocessor_version"));
	spf_header.file_name().originating_system(read_attribute(group, "iso_10303_26_originating_system"));
	spf_header.file_name().authorization(read_attribute(group, "iso_10303_26_authorization"));

	spf_header.write(out);
}

void IfcParse::IfcHdf5File::convert_to_spf(const std::string& name, std::ostream& output) {
	H5::H5File f(name, H5F_ACC_RDONLY);
	
	file_structure fs(f);
	H5Ovisit(f.getId(), H5_INDEX_NAME, H5_ITER_NATIVE, find_groups, &fs);
	fs.finished_schema();
	H5Ovisit(f.getId(), H5_INDEX_NAME, H5_ITER_NATIVE, find_groups, &fs);

	hdf5_output_info info{ f, output };
	for (auto it = fs.begin(); it != fs.end(); ++it) {
		auto& group = it->first;
		auto& schema_id = it->second->first;
		if (schema_id == IfcSchema::Identifier) {
			write_spf_header(group, output);
			H5Ovisit(group.getId(), H5_INDEX_NAME, H5_ITER_NATIVE, iterate, &info);
			output << "ENDSEC;\nEND-ISO-10303-21;\n";
		}
	}
}
