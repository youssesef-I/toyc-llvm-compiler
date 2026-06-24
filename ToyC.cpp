// A toy optimizing compiler: a C-like expression language lowered to LLVM IR,
// with a custom dataflow-based optimization pipeline.
//
// Pipeline:  source -> lex -> parse (AST) -> IR codegen -> [mem2reg, prop, fold] -> IR
//
// The front end emits naive memory-form IR (one alloca per variable, with
// load/store at every use). LLVM's mem2reg promotes that to SSA; the two
// custom passes (constant propagation over a lattice, and constant folding)
// then run on the SSA form. main() is a benchmark harness that reports the
// per-pass instruction-count reduction across a suite of sample programs.

#include <string>
#include <iostream>
#include <vector>
#include <cctype>
#include <memory> 
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Transforms/Utils/Mem2Reg.h>
#include <map>
#include <iomanip>
// A TokenKind classifies every token the lexer can produce.
// Number and Identifier are open-ended and carry a text payload (which
// number? which name?); every other kind is a fixed symbol or keyword
// whose category alone says everything.
enum class TokenKind {
    // Literals and names (carry a text value)
    Number,
    Identifier,

    // Keywords
    Kw_if,
    Kw_else,
    Kw_while,
    Kw_def,
    Kw_return,

    // Operators
    Plus,
    Minus,
    Star,
    Slash,
    Assign,
    EqualEqual,
    Less,
    Greater,

    // Punctuation
    LParen,
    RParen,
    LBrace,
    RBrace,
    Comma,
    Semicolon,

    // Emitted once when input runs out, so the parser can always peek
    // at "the next token" without running off the end of the stream.
    Eof,
};

// One lexical token: its category, the exact characters it came from,
// and where it started in the source.
struct Token {
    TokenKind kind;      // which category this token is
    std::string text;    // the literal characters, e.g. "42" or "if"
    int line, col;       // 1-based source position, kept for error messages
};

// Turn the whole source text into a flat list of tokens, ending in Eof.
std::vector<Token> lex(const std::string& src) {
    std::vector<Token> tokens;
    size_t i = 0;
    int line = 1, col = 1;
    while (i < src.size()) {
        char c = src[i];

        if (std::isspace(static_cast<unsigned char>(c))) { i++; continue; }

        if (std::isdigit(static_cast<unsigned char>(c))) {
            // Maximal munch: consume the whole run of digits.
            size_t start = i;
            int startCol = col;
            while (i < src.size() && std::isdigit(static_cast<unsigned char>(src[i]))) {
                i++;
                col++;
            }
            tokens.push_back(Token{
                TokenKind::Number,
                src.substr(start, i - start),
                line,
                startCol
            });
        } else if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
	    // Maximal munch: consume the whole word, then classify. We lex the
	    // entire identifier first and only then decide keyword vs. name, so
	    // "ifx" lexes as one identifier rather than the keyword "if" + "x".
            size_t start =i;
	    int startCol = col;
	    while (i < src.size() && (std::isalnum(static_cast<unsigned char>(src[i])) || src[i] == '_')) {
		i++;
		col++;
	    }
	    std::string text = src.substr(start, i - start);

	    //Default to Identifier; upgrade to a keyword kind if the word matches one
	    TokenKind kind = TokenKind::Identifier;
	    if		(text == "if") kind = TokenKind::Kw_if;
	    else if (text == "else") kind = TokenKind::Kw_else;
	    else if (text == "while") kind = TokenKind::Kw_while;
	    else if (text == "def") kind = TokenKind::Kw_def;
	    else if (text == "return") kind = TokenKind::Kw_return;


	    tokens.push_back(Token{kind, text, line, startCol});


        } else {
	      switch (c) {
		 case '+':
			tokens.push_back(Token{TokenKind::Plus, "+", line, col});
			i++, col++;
			break;


		 case '-':
    
			tokens.push_back(Token{TokenKind::Minus, "-", line, col});
			i++;col++;
			break;


		case '*':
			tokens.push_back(Token{TokenKind::Star, "*", line, col});
			i++;col++;
			break;


		case '=':
			// Two-character lookahead: '==' is equality, a lone '=' is assignment.
			if(i + 1 < src.size() && src[i + 1] == '=') {
			    tokens.push_back(Token{TokenKind::EqualEqual, "==", line, col});
			i += 2; col += 2;
		    } else {
			tokens. push_back(Token{TokenKind::Assign, "=", line, col});
			i++; col++;
		    }
			break;

		case '<':
			tokens.push_back(Token{TokenKind::Less, "<", line, col});
			i++;col++;
			break;

		case '>' :
			tokens.push_back(Token{TokenKind::Greater, ">", line, col});
			i++;col++;
			break;

		case '/' :
			tokens.push_back(Token{TokenKind::Slash, "/", line, col});
			i++;col++;
			break;

		case '(':
		    tokens.push_back(Token{TokenKind::LParen, "(", line, col});
		    i++,col++;
		    break;

		case ')':
		    tokens.push_back(Token{TokenKind::RParen, ")", line, col});
		    i++;col++;
		    break;
		    
		case '{':
		    tokens.push_back(Token{TokenKind::LBrace, "{", line, col});
		    i++;col++;
		    break;

		case '}':
		    tokens.push_back(Token{TokenKind::RBrace, "}", line, col});
		    i++; col++;
		    break;

		case ',':
		    tokens.push_back(Token{TokenKind::Comma, ",", line, col});
		    i++;col++;
		    break;

		case ';' :
		    tokens.push_back(Token{TokenKind::Semicolon, ";", line, col});
		    i++;col++;
		    break;
	
		default:
			i++; col++; // unrecognized character - skip it
			break;

		    }
		
	      }
    }
    // Sentinel: lets the parser always peek() at "the next token" without
    // a bounds check, and gives loops a guaranteed stopping token.
    tokens.push_back(Token{TokenKind::Eof, "", line, col});
    return tokens;
}

std::string opText(TokenKind k) {
    switch (k) {
	case TokenKind::Plus: return "+";
	case TokenKind::Minus: return "-";
	case TokenKind::Star: return "*";
	case TokenKind::Slash: return "/";
	case TokenKind::Less: return "<";
	case TokenKind::Greater: return ">";
	case TokenKind::EqualEqual: return "==";
	default:		return "?";
    }
}

// Symbol table mapping each source variable name to its stack slot (alloca).
// Global to keep codegen signatures small; reset per compilation in
// compileToFunction(). A production compiler would scope this per function.
static std::map<std::string, llvm::AllocaInst*> namedValues;

// AST expression base. Every node knows how to print itself (for debugging the
// parse) and how to emit its LLVM IR. codegen() returns the Value* holding the
// expression's result, so a parent can wire it into its own instruction.
struct Expr {
    virtual ~Expr() = default;
    virtual void print(int indent) const = 0;
    virtual llvm::Value* codegen(llvm::IRBuilder<>& builder) const = 0;
};

struct NumberExpr : Expr {
    double value;
    NumberExpr(double v) : value(v) {}
    void print(int indent) const override {
	std::cout << std::string(indent * 2, ' ') << "Number(" << value << ")\n";
    }
	llvm::Value* codegen(llvm::IRBuilder<>& builder) const override {
	    // A literal lowers directly to an LLVM constant; no instruction needed.
	    return llvm::ConstantFP::get(llvm::Type::getDoubleTy(builder.getContext()), value);
    }
};

struct VariableExpr : Expr {
    std::string name;
    VariableExpr(std::string n) : name(n) {}
    void print(int indent) const override {
	std::cout << std::string(indent * 2, ' ') << "Variable(" << name << ")\n";
    }
	llvm::Value* codegen(llvm::IRBuilder<>& builder) const override {
	    // Reading a variable is a load from its stack slot. (mem2reg later
	    // removes these loads by promoting the slot to an SSA value.)
	    auto it  = namedValues.find(name);
	    if (it == namedValues.end()) return nullptr;   // use before assignment
	    return builder.CreateLoad(it->second->getAllocatedType(), it->second, name);
    }
};

struct BinaryExpr : Expr { 
    TokenKind op;
    std::unique_ptr<Expr> left;
    std::unique_ptr<Expr> right;
    BinaryExpr(TokenKind o, std::unique_ptr<Expr> l, std::unique_ptr<Expr> r)
	: op(o), left(std::move(l)), right(std::move(r)) {}
    void print(int indent) const override {
	std::cout << std::string(indent * 2, ' ') << "Binary(" << opText(op) << ")\n";
	left->print(indent + 1);
	right->print(indent + 1);
    }
	llvm::Value* codegen(llvm::IRBuilder<>& builder) const override {
	    // Evaluate both operands first, then emit the operator's instruction.
	    llvm::Value* L = left->codegen(builder);
	    llvm::Value* R = right->codegen(builder);
	    switch (op) {
		
		case TokenKind::Plus: 
		    return builder.CreateFAdd(L, R, "addtmp");

		case TokenKind::Minus: 
		    return builder.CreateFSub(L, R, "subtmp");

		case TokenKind::Star: 
		    return builder.CreateFMul(L, R, "multmp");

		case TokenKind::Slash: 
		    return builder.CreateFDiv(L, R, "divtmp");

		// Comparisons produce an i1, but the language is uniformly double,
		// so each result is widened back to 0.0/1.0 with uitofp. The "U"
		// (unordered / unsigned) variants are the conventional default;
		// with no NaN sources in this language the ordering is immaterial.
		case TokenKind::Less: {
		    llvm::Value* cmp = builder.CreateFCmpULT(L, R, "cmptmp");
		    return builder.CreateUIToFP(
			    cmp, llvm::Type::getDoubleTy(builder.getContext()), "booltmp");
		}

		case TokenKind::Greater: {
		    llvm::Value* cmp = builder.CreateFCmpUGT(L, R, "cmptmp");
		    return builder.CreateUIToFP(
			    cmp, llvm::Type::getDoubleTy(builder.getContext()), "booltmp");
		}
		
		case TokenKind::EqualEqual: {
		    llvm::Value* cmp = builder.CreateFCmpUEQ(L, R, "cmptmp");
		    return builder.CreateUIToFP(
			    cmp, llvm::Type::getDoubleTy(builder.getContext()), "booltmp");
		}

		default:			
		    return nullptr;
	    }
    }
};

// Allocate a variable's stack slot at the top of the function's entry block,
// regardless of where the assignment textually appears. This is a hard
// requirement for mem2reg: it only promotes allocas that live in the entry
// block. An alloca emitted inside a branch (e.g. a variable first assigned in
// an `if` arm) would be left in memory and block the SSA/phi construction the
// optimizer depends on. A throwaway builder pinned to entry.begin() does the
// insertion without disturbing the caller's insert point.
 static llvm::AllocaInst* createEntryAlloca(llvm::IRBuilder<>& builder,
                                           const std::string& name) {
    llvm::Function* func = builder.GetInsertBlock()->getParent();
    llvm::BasicBlock& entry = func->getEntryBlock();
    llvm::IRBuilder<> tmp(&entry, entry.begin());
    return tmp.CreateAlloca(llvm::Type::getDoubleTy(builder.getContext()), nullptr, name);
}   

// AST statement base. A statement performs an effect rather than producing a
// value, so codegen() returns void (cf. Expr::codegen returning a Value*).
struct Stmt {
    virtual ~Stmt() = default;
    virtual void print(int indent) const = 0;
    virtual void codegen(llvm::IRBuilder<>& builder) const = 0;
};

struct AssignStmt : Stmt {
    std::string name;
    std::unique_ptr<Expr> value;
    AssignStmt(std::string n, std::unique_ptr<Expr> v)
	: name(std::move(n)), value(std::move(v)) {}
    void print(int indent) const override {
	std::cout << std::string(indent * 2, ' ') << "Assign(" << name << ")\n";
	value->print(indent + 1);
    }
    void codegen(llvm::IRBuilder<>& builder) const override {	
	llvm::Value* val = value ->codegen(builder);
	// Get-or-create the slot: the reference into the map lets a first
	// assignment allocate and record the slot in one step. The slot lives
	// in entry; the store happens here, wherever "here" is in the CFG.
	llvm::AllocaInst*& slot = namedValues[name];
	if (!slot)
	    slot = createEntryAlloca(builder, name);
	builder.CreateStore(val, slot);

    }
};

//return value;
struct ReturnStmt : Stmt {
    std::unique_ptr<Expr> value;
    ReturnStmt(std::unique_ptr<Expr> v) : value(std::move(v)) {}
    void print(int indent) const override {
	std::cout << std::string(indent * 2, ' ') << "Return\n";
	value->print(indent + 1);
    }
    void codegen(llvm::IRBuilder<>& builder) const override {
	llvm::Value* val = value->codegen(builder);
	builder.CreateRet(val);
    }
};

struct IfStmt : Stmt {
    std::unique_ptr<Expr> cond;
    std::vector<std::unique_ptr<Stmt>> thenBody;
    std::vector<std::unique_ptr<Stmt>> elseBody;   // empty when there is no else
    IfStmt(std::unique_ptr<Expr> c,
	    std::vector<std::unique_ptr<Stmt>> t,
	    std::vector<std::unique_ptr<Stmt>> e)
	: cond(std::move(c)), thenBody(std::move(t)), elseBody(std::move(e)) {}
    void print(int indent) const override {
	std::cout << std::string(indent * 2, ' ') << "If\n";
	cond->print(indent + 1);
	std::cout << std::string((indent + 1) * 2, ' ') << "Then\n";
	for (const auto& s : thenBody) s->print(indent +2);
	if (!elseBody.empty()) {
	    std::cout << std::string((indent + 1) * 2, ' ') << "Else\n";
	    for (const auto& s : elseBody) s->print(indent + 2);
	}
    }
    // Lower an if/else to a then/else/merge diamond:
    //
    //        cond
    //       /    \
    //    then     else
    //       \    /
    //       merge   (execution rejoins; rest of the program continues here)
    //
    void codegen(llvm::IRBuilder<>& builder) const override {
	llvm::LLVMContext& ctx = builder.getContext();
	llvm::Function* func = builder.GetInsertBlock()->getParent();

	// The condition is a double; CreateCondBr needs an i1, so test it != 0.
	llvm::Value* condVal = cond->codegen(builder);
	llvm::Value* zero = llvm::ConstantFP::get(llvm::Type::getDoubleTy(ctx), 0.0);
	llvm::Value* condBool = builder.CreateFCmpONE(condVal, zero, "ifcond");


	llvm::BasicBlock* thenBB = llvm::BasicBlock::Create(ctx, "then", func);
	llvm::BasicBlock* elseBB = llvm::BasicBlock::Create(ctx, "else");
	llvm::BasicBlock* mergeBB = llvm::BasicBlock::Create(ctx, "ifcont");

	builder.CreateCondBr(condBool, thenBB, elseBB);

	// then block: emit body, then jump to merge. Every basic block must end
	// in exactly one terminator, so the explicit branch to merge is required
	// even though control "falls through" conceptually.
	builder.SetInsertPoint(thenBB);
	for (const auto& s : thenBody) s->codegen(builder);
	builder.CreateBr(mergeBB);

	// else block (attached now, after then, purely so the IR reads in order).
	func->insert(func->end(), elseBB);
	builder.SetInsertPoint(elseBB);
	for (const auto& s : elseBody) s->codegen(builder);   // empty body is fine
	builder.CreateBr(mergeBB);

	// merge block: leave the builder here so subsequent statements continue
	// after the if. If a variable was assigned on both arms, mem2reg inserts
	// a phi here to reconcile the two incoming values.
	func->insert(func->end(), mergeBB);
	builder.SetInsertPoint(mergeBB);

    }
};

struct WhileStmt : Stmt {
    std::unique_ptr<Expr> cond;
    std::vector<std::unique_ptr<Stmt>> body;
    WhileStmt(std::unique_ptr<Expr> c, std::vector<std::unique_ptr<Stmt>> b)
	: cond(std::move(c)), body(std::move(b)) {}
    void print(int indent) const override {
	std::cout << std::string(indent * 2, ' ') << "While\n";
	cond->print(indent + 1);
	std::cout << std::string((indent + 1) * 2, ' ') << "Do\n";
	for (const auto& s : body) s->print(indent + 2);
    }
// Lower `while` into a condition/body/after CFG shape:
//
//   entry -> cond -----> after
//             |           ^
//             v           |
//            body --------+
//
// The back-edge from body to cond forms the loop. After mem2reg, variables
// updated inside the loop are represented with phi nodes in the condition block.
    void codegen(llvm::IRBuilder<>& builder) const override {
	llvm::LLVMContext& ctx = builder.getContext();
	llvm::Function* func = builder.GetInsertBlock()->getParent();
	

	llvm::BasicBlock* condBB = llvm::BasicBlock::Create(ctx, "cond", func);
	llvm::BasicBlock* loopBB = llvm::BasicBlock::Create(ctx, "loop");
	llvm::BasicBlock* afterBB = llvm::BasicBlock::Create(ctx, "afterloop");

	// Terminate the current block by entering the loop's condition.
	builder.CreateBr(condBB); 
	// cond: evaluate the condition (vs. 0.0 -> i1) and branch into the body
	// or out to after. The conditional branch lives here, not in the entry.
	builder.SetInsertPoint(condBB);
	llvm::Value* condVal = cond->codegen(builder);
	llvm::Value* zero = llvm::ConstantFP::get(llvm::Type::getDoubleTy(ctx), 0.0);
	llvm::Value* condBool = builder.CreateFCmpONE(condVal, zero, "whilecond");
	builder.CreateCondBr(condBool, loopBB, afterBB);

	// loop body: emit it, then the back-edge to cond to re-test.
	func->insert(func->end(), loopBB);
	builder.SetInsertPoint(loopBB);
	for (const auto& s : body) s->codegen(builder);
	builder.CreateBr(condBB);

	// after: the loop exit; the rest of the program continues here.
	func->insert(func->end(), afterBB);
	builder.SetInsertPoint(afterBB);

    }
};

// Recursive-descent parser. One method per grammar level; precedence is encoded
// by the call chain (comparison -> expression -> term -> factor), and left
// associativity by folding each new operator onto the running left operand.
//
// Invariant that keeps every loop here terminating: each iteration consumes at
// least one token (advance() moves pos forward). A loop that could spin without
// advancing would never reach Eof.
struct Parser {
    const std::vector<Token>& tokens;
    size_t pos = 0;

    Parser(const std::vector<Token>& toks) : tokens(toks) {}


    const Token& peek()  { return tokens[pos]; }
    const Token& advance() { return tokens[pos++]; }   // return current, then advance

    // factor := number | identifier | '(' expression ')'
    std::unique_ptr<Expr> parseFactor() {
	const Token& t = peek();
	if (t.kind == TokenKind::Number) {
	    advance();
	    return std::make_unique<NumberExpr>(std::stod(t.text));
	}
	if (t.kind == TokenKind::Identifier) {
	    advance();
	    return std::make_unique<VariableExpr>(t.text);
	}
	if (t.kind == TokenKind::LParen) {
	    advance();
	    std::unique_ptr<Expr> inner = parseExpression();
	    advance();
	    return inner;
	}

	return nullptr;
    }

    // term := factor (('*' | '/') factor)*    -- multiplication binds tighter
    std::unique_ptr<Expr> parseTerm() {
	std::unique_ptr<Expr> left = parseFactor();
	while(peek().kind == TokenKind::Star || peek().kind == TokenKind::Slash) {
	    TokenKind op = advance().kind;
	    std::unique_ptr<Expr> right = parseFactor();
	    // Fold onto the LEFT operand -> left-associative tree.
	    left = std::make_unique<BinaryExpr>(op, std::move(left), std::move(right));
	}
	return left;
    }

    // block := '{' statement* '}'
    std::vector<std::unique_ptr<Stmt>> parseBlock() {
	advance();   // eat '{'
	std::vector<std::unique_ptr<Stmt>> body;
	// The Eof guard is the forward-progress safeguard: a missing '}' would
	// otherwise run this loop off the end of the token stream forever.
	while (peek().kind != TokenKind::RBrace &&
		peek().kind != TokenKind::Eof) {
	    body.push_back(parseStatement());
    }
    advance();   // eat '}'
    return body;
    
    }

    // expression := term (('+' | '-') term)*
    std::unique_ptr<Expr> parseExpression() {
	std::unique_ptr<Expr> left = parseTerm();
	while (peek().kind == TokenKind::Plus || peek().kind == TokenKind::Minus) {
	    TokenKind op = advance().kind;
	    std::unique_ptr<Expr> right = parseTerm();
	    left = std::make_unique<BinaryExpr>(op, std::move(left), std::move(right));
	}
	return left;
    }

    // comparison := expression (('<' | '>' | '==') expression)*
    // Loosest-binding level, so it sits at the top of the expression hierarchy:
    // in `a + 1 < b`, the additions bind first and the comparison sees the sums.
    std::unique_ptr<Expr> parseComparison() {
	std::unique_ptr<Expr> left = parseExpression();
	while (peek().kind == TokenKind::Less ||
		peek().kind == TokenKind::Greater ||
		peek().kind == TokenKind::EqualEqual) {
	    TokenKind op = advance().kind;
	    std::unique_ptr<Expr> right = parseExpression();
	    left = std::make_unique<BinaryExpr>(op, std::move(left), std::move(right));
	}
	return left;
    }

	// statement := 'if' '(' comparison ')' block ('else' block)?
	//            | 'while' '(' comparison ')' block
	//            | 'return' comparison ';'
	//            | identifier '=' comparison ';'
	std::unique_ptr<Stmt> parseStatement() {
	    if (peek().kind == TokenKind::Kw_if) {
		advance();   // 'if'
		advance();   // '('
		auto  cond = parseComparison();
		advance();   // ')'
		auto thenBody = parseBlock();
		std::vector<std::unique_ptr<Stmt>> elseBody;
		if (peek().kind == TokenKind::Kw_else) {
		    advance();
		    elseBody = parseBlock();
		}
		return std::make_unique<IfStmt>(
			std::move(cond), std::move(thenBody), std::move(elseBody));
	    }
	    
	    if(peek().kind == TokenKind::Kw_while) {
		advance();   // 'while'
		advance();   // '('
		auto cond = parseComparison();
		advance();   // ')'
		auto body = parseBlock();
		return std::make_unique<WhileStmt>(std::move(cond), std::move(body));
	    }
	    // return expr ;
	    if (peek().kind == TokenKind::Kw_return) {
		advance();
		auto value = parseComparison();
		advance();
		return std::make_unique<ReturnStmt>(std::move(value));
	    }
	    // identifier = expr ;   two-token lookahead distinguishes assignment
	    // from a bare expression starting with an identifier.
	    if (peek().kind == TokenKind::Identifier
		&& tokens[pos + 1].kind == TokenKind::Assign) {
		std::string name = advance().text;
		advance();
		auto value = parseComparison();
		advance();
		return std::make_unique<AssignStmt>(std::move(name), std::move(value));
	    }
	    return nullptr;
	}
	
	// program := statement*  (until Eof)
	std::vector<std::unique_ptr<Stmt>> parseProgram() {
	    std::vector<std::unique_ptr<Stmt>> program;
	    while (peek().kind != TokenKind::Eof) {
		program.push_back(parseStatement());
	    }
	    return program;
    }
};

// 
// Optimization: constant propagation as an iterative monotone dataflow
// analysis over a flat lattice.
// 

// Lattice element for one SSA value:
//   Top    = "unknown" (optimistic; no evidence seen yet)
//   Const  = a known compile-time constant (the `constant` field)
//   Bottom = "not a constant" (overdefined)
// The order is Top > c > Bottom for every constant c, with distinct constants
// incomparable. Height is 2, that bound guarantees termination below.
struct LatticeVal {
    enum Kind { Top, Const, Bottom };
    Kind kind = Top;
    double constant = 0.0;   // meaningful only when kind == Const
};

// Meet (infimum): how information from two paths combines.
// Top is the identity (no information yields), Bottom absorbs, equal constants
// survive, and disagreeing constants drop to Bottom. Idempotent, commutative,
// and associative, so a phi's incoming values can be folded in any order.
LatticeVal meet(LatticeVal a, LatticeVal b) {
    if (a.kind == LatticeVal::Top) return b;
    if (b.kind == LatticeVal::Top) return a;
    if (a.kind == LatticeVal::Bottom || b.kind == LatticeVal::Bottom)
	return {LatticeVal::Bottom, 0.0};

    if (a.constant == b.constant) return a;
    return {LatticeVal::Bottom, 0.0};
}

// Transfer function: given an instruction and the lattice values computed so
// far, produce the instruction's lattice value. Monotone w.r.t. the order;
// lowering an operand can only lower the result; which is what makes the
// fixed-point iteration well-defined.
LatticeVal transfer(llvm::Instruction* I,
	std::map<llvm::Value*, LatticeVal>& vals) {

    // Resolve any operand to a lattice value: a literal folds straight to
    // Const; an already-analyzed value is looked up; an unseen value is
    // optimistically Top (it may be lowered on a later sweep).
    auto valueOf = [&](llvm::Value* v) -> LatticeVal {
	if (auto* c =llvm::dyn_cast<llvm::ConstantFP>(v))
	    return {LatticeVal::Const, c->getValueAPF().convertToDouble()};
	auto it = vals.find(v);
	if (it != vals.end()) return it-> second;
	return {LatticeVal::Top};
    };

    // A phi's value is the meet of its incoming values. NOTE: an incoming value
    // can be defined later in program order (a loop back-edge), so on the first
    // sweep it reads as Top and the phi is provisional, this is exactly why
    // the analysis must iterate rather than make a single forward pass.
    if (auto* phi = llvm::dyn_cast<llvm::PHINode>(I)) {
	LatticeVal result = { LatticeVal::Top};
	for (unsigned k = 0; k < phi->getNumIncomingValues(); ++k) {
	    LatticeVal incoming = valueOf(phi->getIncomingValue(k));
	    result = meet(result, incoming);
	}
	return result;
    }


    if (auto*binop = llvm::dyn_cast<llvm::BinaryOperator>(I)) {
	LatticeVal a = valueOf(binop->getOperand(0));
	LatticeVal b = valueOf(binop->getOperand(1));

	// Bottom dominates Top: a known-non-constant operand makes the result
	// non-constant regardless of the other; an as-yet-unknown operand (and
	// no Bottom) leaves the result unknown for now.
	if (a.kind == LatticeVal::Bottom || b.kind == LatticeVal::Bottom)
	    return {LatticeVal::Bottom};
	if (a.kind == LatticeVal::Top || b.kind == LatticeVal::Top)
	    return {LatticeVal::Top};

	// Both operands are known constants: compute the result at compile time.
	double result;
	switch (binop->getOpcode()) {
	
	case llvm::Instruction::FAdd: result = a.constant + b.constant; break;
	case llvm::Instruction::FSub: result = a.constant - b.constant; break;
	case llvm::Instruction::FMul: result = a.constant * b.constant; break;
	case llvm::Instruction::FDiv: result = a.constant / b.constant; break;
	    	default : return {LatticeVal::Bottom};
	    }
	return {LatticeVal::Const, result};
    }

    return {LatticeVal::Bottom};   // anything unmodeled is conservatively not-constant
}

// Solve the analysis to a fixed point by Kleene iteration: start every value at
// Top, sweep the whole function applying transfer(), and repeat until a sweep
// changes nothing. Termination is guaranteed by the lattice's height of 2,
// each value descends at most twice (Top -> Const -> Bottom), bounding the
// total number of changes and therefore the number of sweeps.
std::map<llvm::Value*, LatticeVal> analyze(llvm::Function& F) {
    std::map<llvm::Value*, LatticeVal> vals;   // absence == Top (optimistic init)

    bool changed = true;
    while (changed) {
        changed = false;
        for (llvm::BasicBlock& BB : F) {
            for (llvm::Instruction& I : BB) {
                LatticeVal next = transfer(&I, vals);
                LatticeVal prev = vals.count(&I) ? vals[&I]
                                                 : LatticeVal{LatticeVal::Top};
                // A no-op sweep (nothing here changes) is the fixed point.
                if (next.kind != prev.kind || next.constant != prev.constant) {
                    vals[&I] = next;
                    changed = true;
                }
            }
        }
    }
    return vals;
}

	
// Constant folding pass: rewrite any FP binary op whose
// two operands are already literal constants into the computed constant. This
// is the special case of propagation where the constants are visible locally,
// with no dataflow reasoning, kept as a standalone pass and as cleanup.
struct ConstFoldPass : llvm::PassInfoMixin<ConstFoldPass> {
    llvm::PreservedAnalyses run(llvm::Function& F, llvm::FunctionAnalysisManager&) {
        std::vector<llvm::Instruction*> dead;

        for (llvm::BasicBlock& BB : F) {
            for (llvm::Instruction& I : BB) {
                auto* binop = llvm::dyn_cast<llvm::BinaryOperator>(&I);
                if (!binop) continue;                         // not a binary op

                auto* lhs = llvm::dyn_cast<llvm::ConstantFP>(binop->getOperand(0));
		auto* rhs = llvm::dyn_cast<llvm::ConstantFP>(binop->getOperand(1));
		if (!lhs || !rhs) continue;                   // operands not both literal

		double a = lhs->getValueAPF().convertToDouble();
		double b = rhs->getValueAPF().convertToDouble();
		double result;

		switch (binop->getOpcode()) {
		    case llvm::Instruction::FAdd: result = a + b; break;
		    case llvm::Instruction::FSub: result = a - b; break;
		    case llvm::Instruction::FMul: result = a * b; break;
		    case llvm::Instruction::FDiv: result = a / b; break;
		    default: continue;

		}

		llvm::Value* folded = llvm::ConstantFP::get(I.getType(), result);
		I.replaceAllUsesWith(folded);   // rewire every user to the constant
		dead.push_back(&I);             // defer erase to avoid invalidating the iterator
	    }
	}

	for (llvm::Instruction* I : dead) I->eraseFromParent();

	// Report whether the IR changed, so the pass manager can invalidate
	// cached analyses appropriately.
	return dead.empty() ? llvm::PreservedAnalyses::all()
			    : llvm::PreservedAnalyses::none();

		}
};

// Constant propagation pass: run the lattice analysis to a
// fixed point, then replace every value proven constant with its literal.
// Strictly more powerful than ConstFoldPass; it propagates constants through
// chains of SSA values and computes the meet at control-flow merges, folding
// only when sound (and declining when a phi's paths disagree).
struct ConstPropPass : llvm::PassInfoMixin<ConstPropPass> {
    llvm::PreservedAnalyses run(llvm::Function& F, llvm::FunctionAnalysisManager&) {
	std::map<llvm::Value*, LatticeVal> vals = analyze(F);

	std::vector<llvm::Instruction*> dead;

	for (llvm::BasicBlock& BB : F) {
	    for (llvm::Instruction& I : BB) {
	    auto it = vals.find(&I);
	    if (it == vals.end()) continue;
	    if (it->second.kind != LatticeVal::Const) continue;   // only proven constants

	    llvm::Value* c = llvm::ConstantFP::get(I.getType(), it->second.constant);
	    I.replaceAllUsesWith(c);
	    dead.push_back(&I);
	}
    }

    for (llvm::Instruction* I : dead) I->eraseFromParent();

    return dead.empty() ? llvm::PreservedAnalyses::all()
	: llvm::PreservedAnalyses::none();
	}

};

//
// Benchmark harness.
//
// The optimization metric: total instruction count across all blocks.
static int countInstructions(llvm::Function& F) {
    int n = 0;
    for (llvm::BasicBlock& BB : F) n += BB.size();
    return n;
}


// Lex -> parse -> codegen one source string into a fresh @main function.
static llvm::Function* compileToFunction(const std::string& src,
	llvm::Module& module,
	llvm::LLVMContext& context) {
    namedValues.clear();   // isolate each program: the symbol table is global
    auto tokens = lex(src);
    Parser parser(tokens);
    auto program = parser.parseProgram();


    llvm::IRBuilder<> builder(context);
    llvm::FunctionType* funcType =
	llvm::FunctionType::get(llvm::Type::getDoubleTy(context), false);
    llvm::Function* func = llvm::Function::Create(
	    funcType, llvm::Function::ExternalLinkage, "main", &module);
    llvm::BasicBlock* entry = llvm::BasicBlock::Create(context, "entry", func);
    builder.SetInsertPoint(entry);
    for (const auto& stmt : program) stmt->codegen(builder);
    return func;
}

struct Sample { std::string name; std::string src; };

int main() {
    // Each sample is chosen to exercise a different facet of the optimizer.
    // Every program must end in `return` so its final block has a terminator.
    std::vector<Sample> samples = {
        {"arith_chain",     "a = 2; b = a + a; c = b * b; return c;"},          // constants through a chain
        {"branch_agree",    "x = 5; if (x < 100) { y = x; } else { y = x; } return y + 1;"}, // both arms agree
        {"branch_disagree", "a = 4; if (a < 10) { b = a + 1; } else { b = a - 1; } return b * 2;"}, // arms differ -> meet = Bottom
        {"loop_sum",        "x = 10; s = 0; while (x > 0) { s = s + x; x = x - 1; } return s;"},     // runtime-varying loop
    };

    std::cout << std::left << std::setw(18) << "program"
              << std::right << std::setw(7)  << "raw"
              << std::setw(11) << "mem2reg"
              << std::setw(8)  << "prop"
              << std::setw(8)  << "fold"
              << std::setw(13) << "reduction" << "\n";
    std::cout << std::string(65, '-') << "\n";

    int totalRaw = 0, totalFinal = 0;

    for (const auto& s : samples) {
        // Fresh context/module per sample so the duplicate @main names never
        // collide and each compilation is fully independent.
        llvm::LLVMContext context;
        llvm::Module module("bench", context);
        llvm::Function* func = compileToFunction(s.src, module, context);

        int raw = countInstructions(*func);

        llvm::LoopAnalysisManager LAM;
        llvm::FunctionAnalysisManager FAM;
        llvm::CGSCCAnalysisManager CGAM;
        llvm::ModuleAnalysisManager MAM;
        llvm::PassBuilder PB;
        PB.registerModuleAnalyses(MAM);
        PB.registerCGSCCAnalyses(CGAM);
        PB.registerFunctionAnalyses(FAM);
        PB.registerLoopAnalyses(LAM);
        PB.crossRegisterProxies(LAM, FAM, CGAM, MAM);

        // Cumulative measurement: count after each pass so the drop between
        // adjacent columns is that pass's marginal contribution. mem2reg runs
        // first because the propagation pass requires SSA form to see values
        // (without it every operand is a load -> Bottom). prop runs before fold
        // because it is the stronger pass; fold then mops up any leftovers.
        { llvm::FunctionPassManager FPM; FPM.addPass(llvm::PromotePass());  FPM.run(*func, FAM); }
        int afterMem  = countInstructions(*func);

        { llvm::FunctionPassManager FPM; FPM.addPass(ConstPropPass());      FPM.run(*func, FAM); }
        int afterProp = countInstructions(*func);

        { llvm::FunctionPassManager FPM; FPM.addPass(ConstFoldPass());      FPM.run(*func, FAM); }
        int afterFold = countInstructions(*func);

        double pct = raw > 0 ? 100.0 * (raw - afterFold) / raw : 0.0;

        std::cout << std::left << std::setw(18) << s.name
                  << std::right << std::setw(7)  << raw
                  << std::setw(11) << afterMem
                  << std::setw(8)  << afterProp
                  << std::setw(8)  << afterFold
                  << std::setw(12) << std::fixed << std::setprecision(1) << pct << "%" << "\n";

        totalRaw   += raw;
        totalFinal += afterFold;
    }

    std::cout << std::string(65, '-') << "\n";
    double totalPct = totalRaw > 0 ? 100.0 * (totalRaw - totalFinal) / totalRaw : 0.0;
    std::cout << std::left << std::setw(18) << "TOTAL"
              << std::right << std::setw(7)  << totalRaw
              << std::setw(11) << "" << std::setw(8) << "" << std::setw(8) << totalFinal
              << std::setw(12) << std::fixed << std::setprecision(1) << totalPct << "%" << "\n";
}
