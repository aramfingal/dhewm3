/*
mgl_arb.cpp - ARB_vertex_program / ARB_fragment_program assembly to Metal Shading Language.

Every ARB register is a float4. An instruction becomes one MSL statement block:

	{ float4 _r = <expression>; dst.<mask> = _r.<mask>; }

Sources are swizzled float4 expressions; scalar instructions replicate their result. See
mgl_local.h for the variable names the generated body uses. Not supported: ARL and relative
addressing, SWZ, and state bindings other than matrices, texgen planes and texenv colours.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <map>
#include <algorithm>

#include "mgl_local.h"

namespace {

struct Token {
	enum Type { END, IDENT, NUMBER, PUNCT } type;
	std::string text;
	int pos;
};

enum SymKind { SYM_TEMP, SYM_EXPR, SYM_ARRAY, SYM_OUTPUT };

struct Symbol {
	SymKind kind;
	std::string expr;					// r-value (SYM_EXPR), variable (SYM_TEMP / SYM_OUTPUT)
	std::vector<std::string> elements;	// SYM_ARRAY
};

class Translator {
public:
	Translator( const char *text, int length, bool fragment, MglArbProgram &out )
		: src( text, (size_t)length ), frag( fragment ), prog( out ), cur( 0 ), failed( false ) {}

	bool Run();

private:
	std::string src;
	bool frag;
	MglArbProgram &prog;
	std::vector<Token> toks;
	size_t cur;
	bool failed;
	std::map<std::string, Symbol> syms;
	std::string decls;

	void Tokenize();
	const Token &Peek( int ahead = 0 ) const { return toks[ std::min( cur + (size_t)ahead, toks.size() - 1 ) ]; }
	Token Next() { Token t = Peek(); if ( cur < toks.size() - 1 ) cur++; return t; }
	bool IsPunct( const char *p, int ahead = 0 ) const { const Token &t = Peek( ahead ); return t.type == Token::PUNCT && t.text == p; }
	bool Accept( const char *p ) { if ( IsPunct( p ) ) { cur++; return true; } return false; }
	bool Expect( const char *p ) { if ( Accept( p ) ) return true; Fail( std::string( "expected '" ) + p + "'" ); return false; }
	void Fail( const std::string &msg ) {
		if ( !failed ) {
			failed = true;
			prog.error = msg + " near '" + Peek().text + "'";
			prog.errorPos = Peek().pos;
		}
	}
	bool IsIdent( const char *name, int ahead = 0 ) const { const Token &t = Peek( ahead ); return t.type == Token::IDENT && t.text == name; }

	std::string UniformRef( int kind, int a, int b );
	bool ParseIndex( int &index );
	bool ParseNumber( float &value );
	std::string FloatLiteral( float v );
	bool ParseConstantVector( std::string &expr );
	bool ParseBinding( std::string &expr, bool &isOutput, std::vector<std::string> *array );
	bool ParseStateBinding( std::string &expr, std::vector<std::string> *array );
	bool ParseOperand( std::string &expr, bool scalar );
	bool ParseDest( std::string &lvalue, std::string &mask );
	bool ParseSwizzle( std::string &swz, bool allowMask );
	void Statement();
	void Declaration( const std::string &kw );
	void Instruction( const std::string &op );
};

void Translator::Tokenize() {
	size_t i = 0;
	const size_t n = src.size();
	while ( i < n ) {
		const char c = src[i];
		if ( c == '#' ) {
			while ( i < n && src[i] != '\n' ) i++;
			continue;
		}
		if ( isspace( (unsigned char)c ) ) {
			i++;
			continue;
		}
		Token t;
		t.pos = (int)i;
		if ( isalpha( (unsigned char)c ) || c == '_' || c == '$' || c == '!' ) {
			size_t j = i;
			while ( j < n && ( isalnum( (unsigned char)src[j] ) || src[j] == '_' || src[j] == '$' || src[j] == '!' ) ) j++;
			t.type = Token::IDENT;
			t.text = src.substr( i, j - i );
			i = j;
		} else if ( isdigit( (unsigned char)c ) || ( c == '.' && i + 1 < n && isdigit( (unsigned char)src[i + 1] ) ) ) {
			size_t j = i;
			bool seenDot = false;
			while ( j < n ) {
				if ( isdigit( (unsigned char)src[j] ) ) {
					j++;
				} else if ( src[j] == '.' && !seenDot ) {
					// "0..3" is a range, not a number
					if ( j + 1 < n && src[j + 1] == '.' ) break;
					seenDot = true;
					j++;
				} else if ( ( src[j] == 'e' || src[j] == 'E' ) && j + 1 < n &&
							( isdigit( (unsigned char)src[j + 1] ) || ( ( src[j + 1] == '-' || src[j + 1] == '+' ) && j + 2 < n && isdigit( (unsigned char)src[j + 2] ) ) ) ) {
					j += 2;
				} else {
					break;
				}
			}
			t.type = Token::NUMBER;
			t.text = src.substr( i, j - i );
			i = j;
		} else {
			t.type = Token::PUNCT;
			if ( c == '.' && i + 1 < n && src[i + 1] == '.' ) {
				t.text = "..";
				i += 2;
			} else {
				t.text = std::string( 1, c );
				i++;
			}
		}
		toks.push_back( t );
	}
	Token end;
	end.type = Token::END;
	end.text = "<end>";
	end.pos = (int)n;
	toks.push_back( end );
}

std::string Translator::UniformRef( int kind, int a, int b ) {
	MglUniform u;
	u.kind = (uint8_t)kind;
	u.a = (uint8_t)a;
	u.b = (uint16_t)b;
	size_t i;
	for ( i = 0; i < prog.uniforms.size(); i++ ) {
		if ( prog.uniforms[i] == u ) break;
	}
	if ( i == prog.uniforms.size() ) {
		prog.uniforms.push_back( u );
	}
	char buf[32];
	snprintf( buf, sizeof( buf ), "@U%d@", (int)i );
	return buf;
}

bool Translator::ParseIndex( int &index ) {
	if ( !Expect( "[" ) ) return false;
	if ( Peek().type != Token::NUMBER ) { Fail( "expected an index" ); return false; }
	index = atoi( Next().text.c_str() );
	return Expect( "]" );
}

bool Translator::ParseNumber( float &value ) {
	float sign = 1.0f;
	if ( Accept( "-" ) ) sign = -1.0f; else Accept( "+" );
	if ( Peek().type != Token::NUMBER ) { Fail( "expected a number" ); return false; }
	value = sign * (float)atof( Next().text.c_str() );
	return true;
}

std::string Translator::FloatLiteral( float v ) {
	char buf[48];
	snprintf( buf, sizeof( buf ), "%.9g", v );
	if ( !strchr( buf, '.' ) && !strchr( buf, 'e' ) && !strchr( buf, 'n' ) && !strchr( buf, 'i' ) ) {
		strcat( buf, ".0" );
	}
	return buf;
}

// { a } { a, b } { a, b, c } { a, b, c, d }: missing components are 0, 0, 1
bool Translator::ParseConstantVector( std::string &expr ) {
	if ( !Expect( "{" ) ) return false;
	float v[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
	int n = 0;
	do {
		if ( n >= 4 ) { Fail( "too many components" ); return false; }
		if ( !ParseNumber( v[n++] ) ) return false;
	} while ( Accept( "," ) );
	if ( !Expect( "}" ) ) return false;
	expr = "float4(" + FloatLiteral( v[0] ) + ", " + FloatLiteral( v[1] ) + ", " + FloatLiteral( v[2] ) + ", " + FloatLiteral( v[3] ) + ")";
	return true;
}

// state.matrix.<name>[.inverse|.transpose|.invtrans][.row[n]], state.texgen[n].object|eye.s|t|r|q,
// state.texenv[n].color
bool Translator::ParseStateBinding( std::string &expr, std::vector<std::string> *array ) {
	if ( !Expect( "." ) ) return false;
	const std::string what = Next().text;
	if ( what == "matrix" ) {
		if ( !Expect( "." ) ) return false;
		const std::string name = Next().text;
		int matrix = -1;
		int index = 0;
		if ( name == "mvp" ) {
			matrix = MM_MVP;
		} else if ( name == "modelview" ) {
			matrix = MM_MODELVIEW;
			if ( IsPunct( "[" ) && !ParseIndex( index ) ) return false;
		} else if ( name == "projection" ) {
			matrix = MM_PROJECTION;
		} else if ( name == "texture" ) {
			if ( IsPunct( "[" ) && !ParseIndex( index ) ) return false;
			matrix = MM_TEXTURE0 + index;
		} else {
			Fail( "unsupported matrix '" + name + "'" );
			return false;
		}
		int xform = MX_NONE;
		int row = -1;
		while ( IsPunct( "." ) ) {
			const std::string mod = Peek( 1 ).text;
			if ( mod == "inverse" ) { xform = MX_INVERSE; cur += 2; }
			else if ( mod == "transpose" ) { xform = MX_TRANSPOSE; cur += 2; }
			else if ( mod == "invtrans" ) { xform = MX_INVTRANS; cur += 2; }
			else if ( mod == "row" ) { cur += 2; if ( !ParseIndex( row ) ) return false; break; }
			else break;
		}
		if ( row >= 0 ) {
			if ( row > 3 ) { Fail( "matrix row out of range" ); return false; }
			expr = UniformRef( MU_MATRIX_ROW, matrix, row | ( xform << 2 ) );
			return true;
		}
		if ( !array ) { Fail( "a whole matrix can only initialise a PARAM array" ); return false; }
		for ( int r = 0; r < 4; r++ ) {
			array->push_back( UniformRef( MU_MATRIX_ROW, matrix, r | ( xform << 2 ) ) );
		}
		expr.clear();
		return true;
	}
	if ( what == "texgen" ) {
		int unit = 0;
		if ( IsPunct( "[" ) && !ParseIndex( unit ) ) return false;
		if ( !Expect( "." ) ) return false;
		const std::string space = Next().text;
		if ( !Expect( "." ) ) return false;
		const std::string coord = Next().text;
		const char *names = "strq";
		const char *p = coord.size() == 1 ? strchr( names, coord[0] ) : NULL;
		if ( !p || ( space != "object" && space != "eye" ) ) { Fail( "bad texgen binding" ); return false; }
		expr = UniformRef( space == "object" ? MU_TEXGEN_OBJECT : MU_TEXGEN_EYE, unit, (int)( p - names ) );
		return true;
	}
	if ( what == "texenv" ) {
		int unit = 0;
		if ( IsPunct( "[" ) && !ParseIndex( unit ) ) return false;
		if ( !Expect( "." ) ) return false;
		if ( Next().text != "color" ) { Fail( "bad texenv binding" ); return false; }
		expr = UniformRef( MU_ENV_COLOR, unit, 0 );
		return true;
	}
	Fail( "unsupported state binding '" + what + "'" );
	return false;
}

// vertex.*, fragment.*, result.*, program.*, state.*. `array` receives the elements of a
// multi-element binding (program.env[a..b], a whole matrix).
bool Translator::ParseBinding( std::string &expr, bool &isOutput, std::vector<std::string> *array ) {
	isOutput = false;
	const std::string root = Next().text;
	char buf[64];
	if ( root == "program" ) {
		if ( !Expect( "." ) ) return false;
		const std::string which = Next().text;
		if ( which != "env" && which != "local" ) { Fail( "expected env or local" ); return false; }
		const int kind = ( which == "env" ) ? MU_ENV : MU_LOCAL;
		if ( !Expect( "[" ) ) return false;
		if ( Peek().type != Token::NUMBER ) { Fail( "expected an index" ); return false; }
		const int first = atoi( Next().text.c_str() );
		int last = first;
		if ( Accept( ".." ) ) {
			if ( Peek().type != Token::NUMBER ) { Fail( "expected an index" ); return false; }
			last = atoi( Next().text.c_str() );
		}
		if ( !Expect( "]" ) ) return false;
		if ( first < 0 || last < first || last > 255 || ( kind == MU_LOCAL && last > 31 ) ) { Fail( "program parameter index out of range" ); return false; }
		if ( last != first ) {
			if ( !array ) { Fail( "a parameter range can only initialise a PARAM array" ); return false; }
			for ( int i = first; i <= last; i++ ) {
				array->push_back( UniformRef( kind, frag ? 1 : 0, i ) );
			}
			expr.clear();
			return true;
		}
		expr = UniformRef( kind, frag ? 1 : 0, first );
		return true;
	}
	if ( root == "state" ) {
		return ParseStateBinding( expr, array );
	}
	if ( root == "vertex" && !frag ) {
		if ( !Expect( "." ) ) return false;
		const std::string what = Next().text;
		int attr = -1;
		if ( what == "position" ) attr = 0;
		else if ( what == "weight" ) attr = 1;
		else if ( what == "normal" ) attr = 2;
		else if ( what == "color" ) {
			attr = 3;
			if ( IsPunct( "." ) && Peek( 1 ).text == "primary" ) cur += 2;
			else if ( IsPunct( "." ) && Peek( 1 ).text == "secondary" ) { cur += 2; attr = 4; }
		}
		else if ( what == "fogcoord" ) attr = 5;
		else if ( what == "texcoord" ) {
			int unit = 0;
			if ( IsPunct( "[" ) && !ParseIndex( unit ) ) return false;
			attr = 8 + unit;
		}
		else if ( what == "attrib" ) {
			if ( !ParseIndex( attr ) ) return false;
		}
		if ( attr < 0 || attr > 15 ) { Fail( "bad vertex attribute '" + what + "'" ); return false; }
		prog.attribsRead |= 1u << attr;
		snprintf( buf, sizeof( buf ), "a%d", attr );
		expr = buf;
		return true;
	}
	if ( root == "fragment" && frag ) {
		if ( !Expect( "." ) ) return false;
		const std::string what = Next().text;
		if ( what == "color" ) {
			bool secondary = false;
			if ( IsPunct( "." ) && Peek( 1 ).text == "primary" ) cur += 2;
			else if ( IsPunct( "." ) && Peek( 1 ).text == "secondary" ) { cur += 2; secondary = true; }
			prog.varyings |= secondary ? MV_COLOR1 : MV_COLOR0;
			expr = secondary ? "f_col1" : "f_col0";
			return true;
		}
		if ( what == "texcoord" ) {
			int unit = 0;
			if ( IsPunct( "[" ) && !ParseIndex( unit ) ) return false;
			if ( unit < 0 || unit > 7 ) { Fail( "texcoord index out of range" ); return false; }
			prog.varyings |= MV_TEXCOORD0 << unit;
			snprintf( buf, sizeof( buf ), "f_tc%d", unit );
			expr = buf;
			return true;
		}
		if ( what == "fogcoord" ) {
			prog.varyings |= MV_FOG;
			expr = "f_fog";
			return true;
		}
		if ( what == "position" ) {
			prog.readsFragCoord = true;
			expr = "f_pos";
			return true;
		}
		Fail( "bad fragment attribute '" + what + "'" );
		return false;
	}
	if ( root == "result" ) {
		isOutput = true;
		if ( !Expect( "." ) ) return false;
		const std::string what = Next().text;
		if ( frag ) {
			if ( what == "color" ) { expr = "r_col"; return true; }
			if ( what == "depth" ) { prog.writesDepth = true; expr = "r_depth"; return true; }
			Fail( "bad fragment result '" + what + "'" );
			return false;
		}
		if ( what == "position" ) { expr = "o_pos"; return true; }
		if ( what == "color" ) {
			bool secondary = false;
			// result.color[.front|.back][.primary|.secondary]
			while ( IsPunct( "." ) ) {
				const std::string mod = Peek( 1 ).text;
				if ( mod == "front" || mod == "primary" ) cur += 2;
				else if ( mod == "back" ) { cur += 2; }
				else if ( mod == "secondary" ) { cur += 2; secondary = true; }
				else break;
			}
			prog.varyings |= secondary ? MV_COLOR1 : MV_COLOR0;
			expr = secondary ? "o_col1" : "o_col0";
			return true;
		}
		if ( what == "fogcoord" ) { prog.varyings |= MV_FOG; expr = "o_fog"; return true; }
		if ( what == "texcoord" ) {
			int unit = 0;
			if ( IsPunct( "[" ) && !ParseIndex( unit ) ) return false;
			if ( unit < 0 || unit > 7 ) { Fail( "texcoord index out of range" ); return false; }
			prog.varyings |= MV_TEXCOORD0 << unit;
			snprintf( buf, sizeof( buf ), "o_tc%d", unit );
			expr = buf;
			return true;
		}
		if ( what == "pointsize" ) { expr = "o_psize"; return true; }
		Fail( "bad vertex result '" + what + "'" );
		return false;
	}
	Fail( "unknown binding '" + root + "'" );
	return false;
}

// .x / .xyzw (sources) or a write mask (destinations); rgba and stpq are accepted too
bool Translator::ParseSwizzle( std::string &swz, bool allowMask ) {
	swz.clear();
	if ( !IsPunct( "." ) ) return true;
	const Token &t = Peek( 1 );
	if ( t.type != Token::IDENT || t.text.size() > 4 ) return true;
	std::string s;
	for ( size_t i = 0; i < t.text.size(); i++ ) {
		const char c = t.text[i];
		const char *p;
		if ( ( p = strchr( "xyzw", c ) ) != NULL ) s += "xyzw"[p - "xyzw"];
		else if ( ( p = strchr( "rgba", c ) ) != NULL ) s += "xyzw"[p - "rgba"];
		else return true;	// not a swizzle
	}
	if ( !allowMask && s.size() != 1 && s.size() != 4 ) { Fail( "a source swizzle has 1 or 4 components" ); return false; }
	cur += 2;
	swz = s;
	return true;
}

// [-] ( number | {vector} | binding | name[index] ) [.swizzle]
bool Translator::ParseOperand( std::string &expr, bool scalar ) {
	bool negate = false;
	if ( Accept( "-" ) ) negate = true; else Accept( "+" );
	std::string base;
	if ( Peek().type == Token::NUMBER ) {
		const float v = (float)atof( Next().text.c_str() );
		base = "float4(" + FloatLiteral( v ) + ")";
	} else if ( IsPunct( "{" ) ) {
		if ( !ParseConstantVector( base ) ) return false;
	} else if ( Peek().type == Token::IDENT ) {
		const std::string name = Peek().text;
		std::map<std::string, Symbol>::iterator it = syms.find( name );
		if ( it != syms.end() ) {
			cur++;
			const Symbol &s = it->second;
			if ( s.kind == SYM_ARRAY ) {
				if ( !Expect( "[" ) ) return false;
				if ( Peek().type != Token::NUMBER ) { Fail( "only constant array indices are supported" ); return false; }
				const int index = atoi( Next().text.c_str() );
				if ( !Expect( "]" ) ) return false;
				if ( index < 0 || index >= (int)s.elements.size() ) { Fail( "array index out of range" ); return false; }
				base = s.elements[index];
			} else {
				base = s.expr;
			}
		} else if ( name == "vertex" || name == "fragment" || name == "program" || name == "state" || name == "result" ) {
			bool isOutput;
			if ( !ParseBinding( base, isOutput, NULL ) ) return false;
		} else {
			Fail( "undeclared name '" + name + "'" );
			return false;
		}
	} else {
		Fail( "expected an operand" );
		return false;
	}
	std::string swz;
	if ( !ParseSwizzle( swz, false ) ) return false;
	if ( swz.size() == 1 ) {
		swz = std::string( 4, swz[0] );
	}
	expr = base;
	if ( !swz.empty() && swz != "xyzw" ) {
		expr = "(" + expr + ")." + swz;
	}
	if ( negate ) {
		expr = "(-" + expr + ")";
	}
	if ( scalar ) {
		expr = "(" + expr + ").x";
	}
	return true;
}

bool Translator::ParseDest( std::string &lvalue, std::string &mask ) {
	if ( Peek().type != Token::IDENT ) { Fail( "expected a destination" ); return false; }
	const std::string name = Peek().text;
	std::map<std::string, Symbol>::iterator it = syms.find( name );
	if ( it != syms.end() ) {
		cur++;
		if ( it->second.kind != SYM_TEMP && it->second.kind != SYM_OUTPUT ) { Fail( "'" + name + "' cannot be written" ); return false; }
		lvalue = it->second.expr;
	} else if ( name == "result" ) {
		bool isOutput;
		if ( !ParseBinding( lvalue, isOutput, NULL ) ) return false;
	} else {
		Fail( "undeclared destination '" + name + "'" );
		return false;
	}
	if ( !ParseSwizzle( mask, true ) ) return false;
	return true;
}

void Translator::Declaration( const std::string &kw ) {
	if ( kw == "OPTION" ) {
		const std::string opt = Next().text;
		if ( opt == "ARB_position_invariant" ) {
			prog.positionInvariant = true;
		}
		// precision and fog options need nothing here
		Expect( ";" );
		return;
	}
	if ( kw == "TEMP" || kw == "ADDRESS" ) {
		do {
			const std::string name = Next().text;
			Symbol s;
			s.kind = SYM_TEMP;
			s.expr = "t_" + name;
			syms[name] = s;
			decls += "\tfloat4 t_" + name + " = float4(0.0);\n";
		} while ( Accept( "," ) );
		Expect( ";" );
		return;
	}
	const std::string name = Next().text;
	Symbol s;
	if ( kw == "PARAM" ) {
		bool isArray = false;
		if ( Accept( "[" ) ) {
			isArray = true;
			if ( Peek().type == Token::NUMBER ) cur++;
			if ( !Expect( "]" ) ) return;
		}
		if ( !Expect( "=" ) ) return;
		if ( isArray ) {
			s.kind = SYM_ARRAY;
			if ( !Expect( "{" ) ) return;
			do {
				std::string e;
				if ( IsPunct( "{" ) ) {
					if ( !ParseConstantVector( e ) ) return;
					s.elements.push_back( e );
				} else if ( Peek().type == Token::NUMBER || IsPunct( "-" ) ) {
					float v;
					if ( !ParseNumber( v ) ) return;
					s.elements.push_back( "float4(" + FloatLiteral( v ) + ")" );
				} else {
					bool isOutput;
					std::vector<std::string> many;
					if ( !ParseBinding( e, isOutput, &many ) ) return;
					if ( many.empty() ) s.elements.push_back( e );
					else s.elements.insert( s.elements.end(), many.begin(), many.end() );
				}
			} while ( Accept( "," ) );
			if ( !Expect( "}" ) ) return;
		} else {
			s.kind = SYM_EXPR;
			if ( IsPunct( "{" ) ) {
				if ( !ParseConstantVector( s.expr ) ) return;
			} else if ( Peek().type == Token::NUMBER || IsPunct( "-" ) ) {
				float v;
				if ( !ParseNumber( v ) ) return;
				s.expr = "float4(" + FloatLiteral( v ) + ")";
			} else {
				bool isOutput;
				if ( !ParseBinding( s.expr, isOutput, NULL ) ) return;
			}
		}
	} else if ( kw == "ATTRIB" || kw == "OUTPUT" ) {
		if ( !Expect( "=" ) ) return;
		bool isOutput;
		if ( !ParseBinding( s.expr, isOutput, NULL ) ) return;
		s.kind = isOutput ? SYM_OUTPUT : SYM_EXPR;
	} else if ( kw == "ALIAS" ) {
		if ( !Expect( "=" ) ) return;
		const std::string target = Next().text;
		std::map<std::string, Symbol>::iterator it = syms.find( target );
		if ( it == syms.end() ) { Fail( "ALIAS of undeclared name '" + target + "'" ); return; }
		s = it->second;
	}
	syms[name] = s;
	Expect( ";" );
}

void Translator::Instruction( const std::string &opIn ) {
	std::string op = opIn;
	bool sat = false;
	if ( op.size() > 4 && op.compare( op.size() - 4, 4, "_SAT" ) == 0 ) {
		sat = true;
		op.resize( op.size() - 4 );
	}

	if ( op == "KIL" ) {
		std::string v;
		if ( !ParseOperand( v, false ) ) return;
		prog.body += "\tif ( any( " + v + " < float4(0.0) ) ) discard_fragment();\n";
		Expect( ";" );
		return;
	}

	std::string dst, mask;
	if ( !ParseDest( dst, mask ) ) return;
	if ( !Expect( "," ) ) return;

	std::string e;
	if ( op == "TEX" || op == "TXP" || op == "TXB" ) {
		std::string coord;
		if ( !ParseOperand( coord, false ) ) return;
		if ( !Expect( "," ) ) return;
		if ( Next().text != "texture" ) { Fail( "expected texture[n]" ); return; }
		int unit = 0;
		if ( IsPunct( "[" ) && !ParseIndex( unit ) ) return;
		if ( unit < 0 || unit > 15 ) { Fail( "texture image unit out of range" ); return; }
		if ( !Expect( "," ) ) return;
		// "2D" arrives as the number 2 and the name D
		std::string target = Next().text;
		if ( Peek().type == Token::IDENT && Peek().text == "D" && ( target == "1" || target == "2" || target == "3" ) ) {
			target += Next().text;
		}
		int tt = MT_NONE;
		if ( target == "2D" || target == "1D" ) tt = MT_2D;
		else if ( target == "CUBE" ) tt = MT_CUBE;
		else if ( target == "3D" ) tt = MT_3D;
		else if ( target == "RECT" ) tt = MT_RECT;
		else { Fail( "unknown texture target '" + target + "'" ); return; }
		if ( prog.texTarget[unit] != MT_NONE && prog.texTarget[unit] != tt ) { Fail( "one image unit used with two targets" ); return; }
		prog.texTarget[unit] = (uint8_t)tt;
		prog.texMask |= 1u << unit;
		char t[16], s[16];
		snprintf( t, sizeof( t ), "t%d", unit );
		snprintf( s, sizeof( s ), "s%d", unit );
		std::string c = "_c";
		std::string pre = "float4 _c = " + coord + "; ";
		if ( op == "TXP" ) pre += "_c.xyz /= _c.w; ";
		std::string args;
		if ( tt == MT_2D ) args = "_c.xy";
		else if ( tt == MT_RECT ) args = "_c.xy * " + UniformRef( MU_TEX_SIZE, unit, 0 ) + ".xy";
		else args = "_c.xyz";
		if ( op == "TXB" ) args += ", bias( _c.w )";
		prog.body += "\t{ " + pre + "float4 _r = " + t + ".sample( " + s + ", " + args + " ); ";
	} else {
		std::string v[3];
		int numArgs = 2;
		bool scalarArgs = false;
		if ( op == "ABS" || op == "MOV" || op == "FLR" || op == "FRC" || op == "LIT" ) numArgs = 1;
		else if ( op == "RSQ" || op == "RCP" || op == "EXP" || op == "EX2" || op == "LOG" || op == "LG2" ||
				  op == "SIN" || op == "COS" || op == "SCS" ) { numArgs = 1; scalarArgs = true; }
		else if ( op == "POW" ) { numArgs = 2; scalarArgs = true; }
		else if ( op == "MAD" || op == "CMP" || op == "LRP" ) numArgs = 3;
		for ( int i = 0; i < numArgs; i++ ) {
			if ( i > 0 && !Expect( "," ) ) return;
			if ( !ParseOperand( v[i], scalarArgs ) ) return;
		}
		if ( op == "MOV" ) e = v[0];
		else if ( op == "ABS" ) e = "abs( " + v[0] + " )";
		else if ( op == "FLR" ) e = "floor( " + v[0] + " )";
		else if ( op == "FRC" ) e = "fract( " + v[0] + " )";
		else if ( op == "ADD" ) e = v[0] + " + " + v[1];
		else if ( op == "SUB" ) e = v[0] + " - " + v[1];
		else if ( op == "MUL" ) e = v[0] + " * " + v[1];
		else if ( op == "MAD" ) e = v[0] + " * " + v[1] + " + " + v[2];
		else if ( op == "MIN" ) e = "min( " + v[0] + ", " + v[1] + " )";
		else if ( op == "MAX" ) e = "max( " + v[0] + ", " + v[1] + " )";
		else if ( op == "DP3" ) e = "float4( dot( (" + v[0] + ").xyz, (" + v[1] + ").xyz ) )";
		else if ( op == "DP4" ) e = "float4( dot( " + v[0] + ", " + v[1] + " ) )";
		else if ( op == "DPH" ) e = "float4( dot( (" + v[0] + ").xyz, (" + v[1] + ").xyz ) + (" + v[1] + ").w )";
		else if ( op == "XPD" ) e = "float4( cross( (" + v[0] + ").xyz, (" + v[1] + ").xyz ), 0.0 )";
		else if ( op == "SGE" ) e = "select( float4(0.0), float4(1.0), " + v[0] + " >= " + v[1] + " )";
		else if ( op == "SLT" ) e = "select( float4(0.0), float4(1.0), " + v[0] + " < " + v[1] + " )";
		else if ( op == "CMP" ) e = "select( " + v[2] + ", " + v[1] + ", " + v[0] + " < float4(0.0) )";
		else if ( op == "LRP" ) e = "mix( " + v[2] + ", " + v[1] + ", " + v[0] + " )";
		else if ( op == "RSQ" ) e = "float4( rsqrt( abs( " + v[0] + " ) ) )";
		else if ( op == "RCP" ) e = "float4( 1.0 / " + v[0] + " )";
		else if ( op == "POW" ) e = "float4( pow( " + v[0] + ", " + v[1] + " ) )";
		else if ( op == "EXP" || op == "EX2" ) e = "float4( exp2( " + v[0] + " ) )";
		else if ( op == "LOG" ) e = "float4( log2( abs( " + v[0] + " ) ) )";
		else if ( op == "LG2" ) e = "float4( log2( " + v[0] + " ) )";
		else if ( op == "SIN" ) e = "float4( sin( " + v[0] + " ) )";
		else if ( op == "COS" ) e = "float4( cos( " + v[0] + " ) )";
		else if ( op == "SCS" ) e = "float4( cos( " + v[0] + " ), sin( " + v[0] + " ), 0.0, 0.0 )";
		else if ( op == "DST" ) e = "float4( 1.0, (" + v[0] + ").y * (" + v[1] + ").y, (" + v[0] + ").z, (" + v[1] + ").w )";
		else if ( op == "LIT" ) e = "mgl_lit( " + v[0] + " )";
		else { Fail( "unsupported instruction '" + opIn + "'" ); return; }
		prog.body += "\t{ float4 _r = " + e + "; ";
	}
	if ( sat ) {
		prog.body += "_r = saturate( _r ); ";
	}
	if ( dst == "r_depth" ) {
		prog.body += "r_depth.z = _r.z; }\n";
	} else if ( mask.empty() || mask == "xyzw" ) {
		prog.body += dst + " = _r; }\n";
	} else {
		prog.body += dst + "." + mask + " = _r." + mask + "; }\n";
	}
	Expect( ";" );
}

bool Translator::Run() {
	Tokenize();
	prog.isFragment = frag;
	const std::string header = Next().text;
	if ( header.compare( 0, 7, frag ? "!!ARBfp" : "!!ARBvp" ) != 0 ) {
		cur = 0;
		Fail( frag ? "missing !!ARBfp header" : "missing !!ARBvp header" );
		return false;
	}
	// "!!ARBvp1.0": the version digits follow as a number token
	if ( Peek().type == Token::NUMBER ) cur++;

	while ( !failed ) {
		const Token t = Next();
		if ( t.type == Token::END ) { Fail( "missing END" ); break; }
		if ( t.type != Token::IDENT ) { cur--; Fail( "expected a statement" ); break; }
		if ( t.text == "END" ) break;
		if ( t.text == "OPTION" || t.text == "TEMP" || t.text == "PARAM" || t.text == "ATTRIB" ||
			 t.text == "OUTPUT" || t.text == "ALIAS" || t.text == "ADDRESS" ) {
			Declaration( t.text );
		} else if ( t.text == "ARL" || t.text == "SWZ" ) {
			cur--;
			Fail( "unsupported instruction '" + t.text + "'" );
		} else {
			Instruction( t.text );
		}
	}
	if ( failed ) {
		return false;
	}
	prog.body = decls + prog.body;
	return true;
}

} // namespace

bool MglArb_Translate( const char *text, int length, bool fragment, MglArbProgram &out ) {
	out.valid = false;
	out.error.clear();
	out.errorPos = -1;
	out.body.clear();
	out.uniforms.clear();
	out.positionInvariant = false;
	out.writesDepth = false;
	out.readsFragCoord = false;
	out.attribsRead = 0;
	out.varyings = 0;
	out.texMask = 0;
	for ( int i = 0; i < 16; i++ ) out.texTarget[i] = MT_NONE;

	Translator t( text, length, fragment, out );
	out.valid = t.Run();
	return out.valid;
}
