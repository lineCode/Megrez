/* =====================================================================
Copyright 2017 The Megrez Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

	http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
========================================================================*/
// The implementation of parser in `idl.h`

#include "megrez/basic.h"
#include "megrez/builder.h"
#include "megrez/info.h"
#include "megrez/string.h"
#include "megrez/struct.h"
#include "megrez/vector.h"
#include "megrez/util.h"
#include "compiler/idl.h"

namespace megrez {

const char *const kTypeNames[] = {
	#define MEGREZ_TD(ENUM, IDLTYPE, CTYPE) IDLTYPE,
		MEGREZ_GEN_TYPES(MEGREZ_TD)
	#undef MEGREZ_TD
	nullptr
};

const char kTypeSizes[] = {
	#define MEGREZ_TD(ENUM, IDLTYPE, CTYPE) sizeof(CTYPE),
		MEGREZ_GEN_TYPES(MEGREZ_TD)
	#undef MEGREZ_TD
};

static void Error(const std::string &msg) { throw msg; }

// Ensure that integer values we parse fit inside the declared integer type.
static void CheckBitsFit(int64_t val, size_t bits) {
	auto mask = (1ll << bits) - 1;  // Bits we allow to be used.
	if (bits < 64 &&
		(val & ~mask) != 0 &&  // Positive or unsigned.
		(val |  mask) != -1)   // Negative.
		Error("Constant does not fit in a " + NumToString(bits) + "-bit field");
}

// atot: templated version of atoi/atof: convert a string to an instance of T.
template<typename T> 
inline T atot(const char *s) {
	auto val = StringToInt(s);
	CheckBitsFit(val, sizeof(T) * 8);
	return (T)val;
}
template<> 
inline bool atot<bool>(const char *s) { return 0 != atoi(s); }
template<> 
inline float atot<float>(const char *s) { return static_cast<float>(strtod(s, nullptr)); }
template<> 
inline double atot<double>(const char *s) { return strtod(s, nullptr); }
template<> 
inline Offset<void> atot<Offset<void>>(const char *s) { return Offset<void>(atoi(s)); }

#define MEGREZ_GEN_TOKENS(TD) \
	TD(Eof, 256, "end of file") \
	TD(StringConstant, 257, "string constant") \
	TD(IntegerConstant, 258, "integer constant") \
	TD(FloatConstant, 259, "float constant") \
	TD(Identifier, 260, "identifier") \
	TD(Info, 261, "info") \
	TD(Struct, 262, "struct") \
	TD(Enum, 263, "enum") \
	TD(Union, 264, "union") \
	TD(NameSpace, 265, "namespace") \
	TD(MainType, 266, "Main")
enum {
	#define MEGREZ_TOKEN(NAME, VALUE, STRING) kToken ## NAME,
		MEGREZ_GEN_TOKENS(MEGREZ_TOKEN)
	#undef MEGREZ_TOKEN
	#define MEGREZ_TD(ENUM, IDLTYPE, CTYPE) kToken ## ENUM,
		MEGREZ_GEN_TYPES(MEGREZ_TD)
	#undef MEGREZ_TD
};

static std::string TokenToString(int t) {
	static const char *tokens[] = {
		#define MEGREZ_TOKEN(NAME, VALUE, STRING) STRING,
			MEGREZ_GEN_TOKENS(MEGREZ_TOKEN)
		#undef MEGREZ_TOKEN
		#define MEGREZ_TD(ENUM, IDLTYPE, CTYPE) IDLTYPE,
			MEGREZ_GEN_TYPES(MEGREZ_TD)
		#undef MEGREZ_TD
	};
	if (t < 256) {  // A single ascii char token.
		std::string s;
		s.append(1, t);
		return s;
	} else { return tokens[t - 256]; } // Other tokens.
}

void Parser::Next() {
	doc_comment_.clear();
	bool seen_newline = false;
	for (;;) {
		char c = *cursor_++;
		token_ = c;
		switch (c) {
			case '\0': cursor_--; token_ = kTokenEof; return;
			case ' ': case '\r': case '\t': break;
			case '\n': line_++; seen_newline = true; break;
			case '{': case '}': case '(': case ')': case '[': case ']': return;
			case ',': case ':': case ';': case '=': return;
			case '.':
				if(!isdigit(*cursor_)) return;
				Error("Floating point constant can\'t start with \".\"");
				break;
			case '\"':
				attribute_ = "";
				while (*cursor_ != '\"') {
					if (*cursor_ < ' ' && *cursor_ >= 0)
						Error("Illegal character in string constant");
					if (*cursor_ == '\\') {
						cursor_++;
						switch (*cursor_) {
							case 'n':  attribute_ += '\n'; cursor_++; break;
							case 't':  attribute_ += '\t'; cursor_++; break;
							case 'r':  attribute_ += '\r'; cursor_++; break;
							case '\"': attribute_ += '\"'; cursor_++; break;
							case '\\': attribute_ += '\\'; cursor_++; break;
							default: Error("Unknown escape code in string constant"); break;
						}
					} else {
						attribute_ += *cursor_++;
					}
				}
				cursor_++;
				token_ = kTokenStringConstant;
				return;
			case '/':
				if (*cursor_ == '/') {
					const char *start = ++cursor_;
					while (*cursor_ && *cursor_ != '\n') cursor_++;
					if (*start == '/') {  // documentation comment
						if (!seen_newline)
							Error("A documentation comment should be on a line on its own");
						// TODO: To support multiline comments.
						doc_comment_ += std::string(start + 1, cursor_);
					}
					break;
				}
				// fall thru
			default:
				if (isalpha(static_cast<unsigned char>(c))) {
					// Collect all chars of an identifier:
					const char *start = cursor_ - 1;
					while (isalnum(static_cast<unsigned char>(*cursor_)) ||
								 *cursor_ == '_')
						cursor_++;
					attribute_.clear();
					attribute_.append(start, cursor_);
					// First, see if it is a type keyword from the info of types:
					#define MEGREZ_TD(ENUM, IDLTYPE, CTYPE) \
						if (attribute_ == IDLTYPE) { \
							token_ = kToken ## ENUM; \
							return; \
						}
						MEGREZ_GEN_TYPES(MEGREZ_TD)
					#undef MEGREZ_TD
					// If it's a boolean constant keyword, turn those into integers,
					// which simplifies our logic downstream.
					if (attribute_ == "true" || attribute_ == "false") {
						attribute_ = NumToString(attribute_ == "true");
						token_ = kTokenIntegerConstant;
						return;
					}
					// Check for declaration keywords:
					if (attribute_ == "info")     { token_ = kTokenInfo;     return; }
					if (attribute_ == "struct")    { token_ = kTokenStruct;    return; }
					if (attribute_ == "enum")      { token_ = kTokenEnum;      return; }
					if (attribute_ == "union")     { token_ = kTokenUnion;     return; }
					if (attribute_ == "namespace") { token_ = kTokenNameSpace; return; }
					if (attribute_ == "Main") { token_ = kTokenMainType;  return; }
					// If not, it is a user-defined identifier:
					token_ = kTokenIdentifier;
					return;
				} else if (isdigit(static_cast<unsigned char>(c)) || c == '-') {
					const char *start = cursor_ - 1;
					while (isdigit(static_cast<unsigned char>(*cursor_))) cursor_++;
					if (*cursor_ == '.') {
						cursor_++;
						while (isdigit(static_cast<unsigned char>(*cursor_))) cursor_++;
						token_ = kTokenFloatConstant;
					} else {
						token_ = kTokenIntegerConstant;
					}
					attribute_.clear();
					attribute_.append(start, cursor_);
					return;
				}
				std::string ch;
				ch = c;
				if (c < ' ' || c > '~') ch = "code: " + NumToString(c);
				Error("Illegal character: " + ch);
				break;
		}
	}
}

bool Parser::IsNext(int t) {
	bool isnext = t == token_;
	if (isnext) Next();
	return isnext;
}

void Parser::Expect(int t) {
	if (t != token_) 
		Error("Expecting: " + TokenToString(t) + " instead got: " + TokenToString(token_));
	Next();
}

// Parse any IDL type.
void Parser::ParseType(Type &type) {
	if (token_ >= kTokenBOOL && token_ <= kTokenSTRING) {
		type.base_type = static_cast<BaseType>(token_ - kTokenNONE);
	} else {
		if (token_ == kTokenIdentifier) {
			auto enum_def = enums_.Lookup(attribute_);
			if (enum_def) {
				type = enum_def->underlying_type;
				if (enum_def->is_union) type.base_type = BASE_TYPE_UNION;
			} else {
				type.base_type = BASE_TYPE_STRUCT;
				type.struct_def = LookupCreateStruct(attribute_);
			}
		} else if (token_ == '[') {
			Next();
			Type subtype;
			ParseType(subtype);
			if (subtype.base_type == BASE_TYPE_VECTOR) {
				// We could support this, but it will complicate things, and it's
				// easier to work around with a struct around the inner vector.
				Error("Nested vector types not supported (wrap in info first).");
			}
			if (subtype.base_type == BASE_TYPE_UNION) {
				// We could support this if we stored a struct of 2 elements per
				// union element.
				Error("Vector of union types not supported (wrap in info first).");
			}
			type = Type(BASE_TYPE_VECTOR, subtype.struct_def);
			type.element = subtype.base_type;
			Expect(']');
			return;
		} else {
			Error("Illegal type syntax");
		}
	}
	Next();
}

FieldDef &Parser::AddField(StructDef &struct_def, const std::string &name, const Type &type) {
	auto &field = *new FieldDef();
	field.value.offset =
		FieldIndexToOffset(static_cast<vofs_t>(struct_def.fields.vec.size()));
	field.name = name;
	field.value.type = type;
	if (struct_def.fixed) {
		auto size = InlineSize(type);
		auto alignment = InlineAlignment(type);
		struct_def.minalign = std::max(struct_def.minalign, alignment);
		struct_def.PadLastField(alignment);
		field.value.offset = static_cast<uofs_t>(struct_def.bytesize);
		struct_def.bytesize += size;
	}
	if (struct_def.fields.Add(name, &field))
		Error("Field already exists: " + name);
	return field;
}

void Parser::ParseField(StructDef &struct_def) {
	std::string name = attribute_;
	std::string dc = doc_comment_;
	Expect(kTokenIdentifier);
	Expect(':');
	Type type;
	ParseType(type);

	if (struct_def.fixed && 
		!IsScalar(type.base_type) && 
		!IsStruct(type)) {
		Error("structs_ may contain only scalar or struct fields");
	}

	if (type.base_type == BASE_TYPE_UNION) {
		AddField(struct_def, name + "_type", type.enum_def->underlying_type);
	}

	auto &field = AddField(struct_def, name, type);

	if (token_ == '=') {
		Next();
		ParseSingleValue(field.value);
	}

	field.doc_comment = dc;
	ParseMetaData(field);
	field.deprecated = field.attributes.Lookup("deprecated") != nullptr;
	if (field.deprecated && struct_def.fixed)
		Error("Cannot deprecate fields in a struct");
	Expect(';');
}

void Parser::ParseAnyValue(Value &val, FieldDef *field) {
	switch (val.type.base_type) {
		case BASE_TYPE_UNION: {
			assert(field);
			if (!field_stack_.size() ||
				 field_stack_.back().second->value.type.base_type != BASE_TYPE_UTYPE)
				Error("Missing type field before this union value: " + field->name);
			auto enum_idx = atot<unsigned char>(
																		field_stack_.back().first.constant.c_str());
			auto struct_def = val.type.enum_def->ReverseLookup(enum_idx);
			if (!struct_def) Error("Illegal type id for: " + field->name);
			val.constant = NumToString(ParseInfo(*struct_def));
			break;
		}
		case BASE_TYPE_STRUCT:
			val.constant = NumToString(ParseInfo(*val.type.struct_def));
			break;
		case BASE_TYPE_STRING: {
			auto s = attribute_;
			Expect(kTokenStringConstant);
			val.constant = NumToString(builder_.CreateString(s).o);
			break;
		}
		case BASE_TYPE_VECTOR: {
			Expect('[');
			val.constant = NumToString(ParseVector(val.type.VectorType()));
			break;
		}
		default:
			ParseSingleValue(val);
			break;
	}
}

void Parser::SerializeStruct(const StructDef &struct_def, const Value &val) {
	auto off = atot<uofs_t>(val.constant.c_str());
	assert(struct_stack_.size() - off == struct_def.bytesize);
	builder_.Align(struct_def.minalign);
	builder_.PushBytes(&struct_stack_[off], struct_def.bytesize);
	struct_stack_.resize(struct_stack_.size() - struct_def.bytesize);
	builder_.AddStructOffset(val.offset, builder_.GetSize());
}

uofs_t Parser::ParseInfo(const StructDef &struct_def) {
	Expect('{');
	size_t fieldn = 0;
	for (;;) {
		std::string name = attribute_;
		if (!IsNext(kTokenStringConstant)) Expect(kTokenIdentifier);
		auto field = struct_def.fields.Lookup(name);
		if (!field) Error("Unknown field: " + name);
		if (struct_def.fixed && (fieldn >= struct_def.fields.vec.size()
			|| struct_def.fields.vec[fieldn] != field)) {
			 Error("Struct field appearing out of order: " + name);
		}
		Expect(':');
		Value val = field->value;
		ParseAnyValue(val, field);
		field_stack_.push_back(std::make_pair(val, field));
		fieldn++;
		if (IsNext('}')) break;
		Expect(',');
	}
	if (struct_def.fixed && fieldn != struct_def.fields.vec.size())
		Error("Incomplete struct initialization: " + struct_def.name);
	auto start = struct_def.fixed
				? builder_.StartStruct(struct_def.minalign)
				: builder_.StartInfo();

	for (size_t size = struct_def.sortbysize ? sizeof(max_scalar_t) : 1;
			 size;
			 size /= 2) {
		// Go through elements in reverse, since we're building the data backwards.
		for (auto it = field_stack_.rbegin();
			 it != field_stack_.rbegin() + fieldn; ++it) {
			auto &value = it->first;
			auto field = it->second;
			if (!struct_def.sortbysize || size == SizeOf(value.type.base_type)) {
				switch (value.type.base_type) {
					#define MEGREZ_TD(ENUM, IDLTYPE, CTYPE) \
						case BASE_TYPE_ ## ENUM: \
							builder_.Pad(field->padding); \
							builder_.AddElement(value.offset, \
												atot<CTYPE>(value.constant.c_str()), \
												atot<CTYPE>(field->value.constant.c_str())); \
							break;
						MEGREZ_GEN_TYPES_SCALAR(MEGREZ_TD);
					#undef MEGREZ_TD
					#define MEGREZ_TD(ENUM, IDLTYPE, CTYPE) \
						case BASE_TYPE_ ## ENUM: \
							builder_.Pad(field->padding); \
							if (IsStruct(field->value.type)) { \
								SerializeStruct(*field->value.type.struct_def, value); \
							} else { \
								builder_.AddOffset(value.offset, \
								atot<CTYPE>(value.constant.c_str())); \
							} \
							break;
						MEGREZ_GEN_TYPES_POINTER(MEGREZ_TD);
					#undef MEGREZ_TD
				}
			}
		}
	}
	for (size_t i = 0; i < fieldn; i++) field_stack_.pop_back();

	if (struct_def.fixed) {
		builder_.ClearOffsets();
		builder_.EndStruct();
		// Temporarily store this struct in a side buffer, since this data has to
		// be stored in-line later in the parent object.
		auto off = struct_stack_.size();
		struct_stack_.insert(struct_stack_.end(),
							 builder_.GetBufferPointer(),
							 builder_.GetBufferPointer() + struct_def.bytesize);
		builder_.PopBytes(struct_def.bytesize);
		return static_cast<uofs_t>(off);
	} else { 
		return builder_.EndInfo( start, static_cast<vofs_t>(struct_def.fields.vec.size()));
	}
}

uofs_t Parser::ParseVector(const Type &type) {
	int count = 0;
	if (token_ != ']') for (;;) {
		Value val;
		val.type = type;
		ParseAnyValue(val, NULL);
		field_stack_.push_back(std::make_pair(val, nullptr));
		count++;
		if (token_ == ']') break;
		Expect(',');
	}
	Next();

	builder_.StartVector(count * InlineSize(type), InlineAlignment((type)));
	for (int i = 0; i < count; i++) {
		// start at the back, since we're building the data backwards.
		auto &val = field_stack_.back().first;
		switch (val.type.base_type) {
			#define MEGREZ_TD(ENUM, IDLTYPE, CTYPE) \
				case BASE_TYPE_ ## ENUM: \
					if (IsStruct(val.type)) SerializeStruct(*val.type.struct_def, val); \
					else builder_.PushElement(atot<CTYPE>(val.constant.c_str())); \
					break;
				MEGREZ_GEN_TYPES(MEGREZ_TD)
			#undef MEGREZ_TD
		}
		field_stack_.pop_back();
	}

	builder_.ClearOffsets();
	return builder_.EndVector(count);
}

void Parser::ParseMetaData(Definition &def) {
	if (IsNext('(')) {
		for (;;) {
			auto name = attribute_;
			Expect(kTokenIdentifier);
			auto e = new Value();
			def.attributes.Add(name, e);
			if (IsNext(':')) { ParseSingleValue(*e); }
			if (IsNext(')')) { break; }
			Expect(',');
		}
	}
}

bool Parser::TryTypedValue(int dtoken, bool check, Value &e, BaseType req) {
	bool match = dtoken == token_;
	if (match) {
		e.constant = attribute_;
		if (!check) {
			if (e.type.base_type == BASE_TYPE_NONE) {
				e.type.base_type = req;
			} else {
				Error(std::string("Type mismatch: expecting: ") +
					  kTypeNames[e.type.base_type] +
					  ", found: " +
					  kTypeNames[req]);
			}
		}
		Next();
	}
	return match;
}

void Parser::ParseSingleValue(Value &e) {
	if (TryTypedValue(kTokenIntegerConstant,
					  IsScalar(e.type.base_type), e,
					  BASE_TYPE_INT) ||
			TryTypedValue(kTokenFloatConstant,
					  IsFloat(e.type.base_type), e,
					  BASE_TYPE_FLOAT) ||
			TryTypedValue(kTokenStringConstant,
					  e.type.base_type == BASE_TYPE_STRING, e,
					  BASE_TYPE_STRING)) {
	} else if (token_ == kTokenIdentifier) {
		for (auto it = enums_.vec.begin(); it != enums_.vec.end(); ++it) {
			auto ev = (*it)->vals.Lookup(attribute_);
			if (ev) {
				attribute_ = NumToString(ev->value);
				TryTypedValue(kTokenIdentifier,
							  IsInteger(e.type.base_type), e,
							  BASE_TYPE_INT);
				return;
			}
		}
		Error("Not valid enum value: " + attribute_);
	} else {
		Error("Cannot parse value starting with: " + TokenToString(token_));
	}
}

StructDef *Parser::LookupCreateStruct(const std::string &name) {
	auto struct_def = structs_.Lookup(name);
	if (!struct_def) {
		// Rather than failing, we create a "pre declared" StructDef, due to
		// circular references, and check for errors at the end of parsing.
		struct_def = new StructDef();
		structs_.Add(name, struct_def);
		struct_def->name = name;
		struct_def->predecl = true;
	}
	return struct_def;
}

void Parser::ParseEnum(bool is_union) {
	std::string dc = doc_comment_;
	Next();
	std::string name = attribute_;
	Expect(kTokenIdentifier);
	auto &enum_def = *new EnumDef();
	enum_def.name = name;
	enum_def.doc_comment = dc;
	enum_def.is_union = is_union;
	if (enums_.Add(name, &enum_def)) Error("Enum already exists: " + name);
	if (is_union) {
		enum_def.underlying_type.base_type = BASE_TYPE_UTYPE;
		enum_def.underlying_type.enum_def = &enum_def;
	} else if (IsNext(':')) {
		// short is the default type for fields when you use enums,
		// though people are encouraged to pick any integer type instead.
		ParseType(enum_def.underlying_type);
		if (!IsInteger(enum_def.underlying_type.base_type))
			Error("Underlying enum type must be integral");
	} else {
		enum_def.underlying_type.base_type = BASE_TYPE_SHORT;
	}
	ParseMetaData(enum_def);
	Expect('{');
	if (is_union) enum_def.vals.Add("NONE", new EnumVal("NONE", 0));
	do {
		std::string name = attribute_;
		std::string dc = doc_comment_;
		Expect(kTokenIdentifier);
		auto prevsize = enum_def.vals.vec.size();
		auto &ev = *new EnumVal(name, static_cast<int>(
											enum_def.vals.vec.size()
												? enum_def.vals.vec.back()->value + 1
												: 0));
		if (enum_def.vals.Add(name, &ev))
			Error("Enum value already exists: " + name);
		ev.doc_comment = dc;
		if (is_union) {
			ev.struct_def = LookupCreateStruct(name);
		}
		if (IsNext('=')) {
			ev.value = atoi(attribute_.c_str());
			Expect(kTokenIntegerConstant);
			if (prevsize && enum_def.vals.vec[prevsize - 1]->value >= ev.value)
				Error("Enum values must be specified in ascending order");
		}
	} while (IsNext(','));
	Expect('}');
}

void Parser::ParseDecl() {
	std::string dc = doc_comment_;
	bool fixed = IsNext(kTokenStruct);
	if (!fixed) Expect(kTokenInfo);
	std::string name = attribute_;
	Expect(kTokenIdentifier);
	auto &struct_def = *LookupCreateStruct(name);
	if (!struct_def.predecl) Error("Datatype already exists: " + name);
	struct_def.predecl = false;
	struct_def.name = name;
	struct_def.doc_comment = dc;
	struct_def.fixed = fixed;
	// Move this struct to the back of the vector just in case it was predeclared,
	// to preserve declartion order.
	remove(structs_.vec.begin(), structs_.vec.end(), &struct_def);
	structs_.vec.back() = &struct_def;
	ParseMetaData(struct_def);
	struct_def.sortbysize =
		struct_def.attributes.Lookup("Original_order") == nullptr && !fixed;
	Expect('{');
	while (token_ != '}') ParseField(struct_def);
	struct_def.PadLastField(struct_def.minalign);
	Expect('}');
	auto force_align = struct_def.attributes.Lookup("Force_align");
	if (fixed && force_align) {
		auto align = static_cast<size_t>(atoi(force_align->constant.c_str()));
		if (force_align->type.base_type != BASE_TYPE_INT ||
				align < struct_def.minalign ||
				align > 256 ||
				align & (align - 1))
			Error("Force_align must be a power of two integer ranging from the"
						"struct\'s natural alignment to 256");
		struct_def.minalign = align;
	}
}

bool Parser::SetMainType(const char *name) {
	main_struct_def = structs_.Lookup(name);
	return main_struct_def != nullptr;
}

bool Parser::Parse(const char *source) {
	source_ = cursor_ = source;
	line_ = 1;
	error_.clear();
	builder_.Clear();
	try {
		Next();
		while (token_ != kTokenEof) {
			if (token_ == kTokenNameSpace) {
				Next();
				for (;;) {
					name_space_.push_back(attribute_);
					Expect(kTokenIdentifier);
					if (!IsNext('.')) break;
				}
				Expect(';');
			} else if (token_ == '{') {
				if (!main_struct_def) Error("No main type set to parse json with");
				if (builder_.GetSize()) {
					Error("Cannot have more than one json object in a file");
				}
				builder_.Finish(Offset<Info>(ParseInfo(*main_struct_def)));
			} else if (token_ == kTokenEnum) {
				ParseEnum(false);
			} else if (token_ == kTokenUnion) {
				ParseEnum(true);
			} else if (token_ == kTokenMainType) {
				Next();
				auto Main = attribute_;
				Expect(kTokenIdentifier);
				Expect(';');
				if (!SetMainType(Main.c_str()))
					Error("Unknown main type: " + Main);
				if (main_struct_def->fixed)
					Error("Main type must be a info");
			} else {
				ParseDecl();
			}
		}
		for (auto it = structs_.vec.begin(); it != structs_.vec.end(); ++it) {
			if ((*it)->predecl)
				Error("Type referenced but not defined: " + (*it)->name);
		}
		for (auto it = enums_.vec.begin(); it != enums_.vec.end(); ++it) {
			auto &enum_def = **it;
			if (enum_def.is_union) {
				for (auto it = enum_def.vals.vec.begin();
					 it != enum_def.vals.vec.end();
					 ++it) {
					auto &val = **it;
					if (val.struct_def && val.struct_def->fixed)
						Error("Only info can be union elements: " + val.name);
				}
			}
		}
	} catch (const std::string &msg) {
		error_ = "Line " + NumToString(line_) + ": " + msg;
		return false;
	}
	assert(!struct_stack_.size());
	return true;
}

}  // namespace megrez