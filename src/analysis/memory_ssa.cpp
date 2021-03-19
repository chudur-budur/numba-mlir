#include "plier/analysis/memory_ssa.hpp"

#include <mlir/Dialect/SCF/SCF.h>
#include <mlir/IR/BuiltinOps.h>

struct plier::MemorySSA::Node : public llvm::ilist_node<Node>
{
    using Type = plier::MemorySSA::NodeType;

    mlir::Operation* getOperation() const
    {
        return operation;
    }

    Type getType() const
    {
        return type;
    }

    unsigned getNumArguments() const
    {
        return argCount;
    }

    auto getArguments()
    {
        return llvm::map_range(llvm::makeArrayRef(&args[0], argCount), [](auto& a)->Node*
        {
            return a.arg;
        });
    }

    auto getUsers()
    {
        return llvm::map_range(users, [](auto& a)->Node*
        {
            return a.getParent();
        });
    }

    auto getUses()
    {
        struct Use
        {
            Node* user;
            unsigned index;
        };

        return llvm::map_range(users, [](auto& a)->Use
        {
            return {a.getParent(), a.index};
        });
    }

    Node* getArgument(unsigned i)
    {
        assert(i < argCount);
        return args[i].arg;
    }

    void setArgument(unsigned i, Node* node)
    {
        assert(i < argCount);
        if (nullptr != args[i].arg)
        {
            args[i].arg->users.erase(args[i].getIterator());
        }
        args[i].arg = node;
        if (nullptr != node)
        {
            node->users.push_back(args[i]);
        }
    }

private:
    Node() = default;
    Node(const Node&) = delete;
    Node(mlir::Operation* op, Type t, llvm::ArrayRef<Node*> a)
    {
        assert(nullptr != op);
        assert(a.size() == 1 || t == Type::Phi);
        operation = op;
        argCount = static_cast<unsigned>(a.size());
        type = t;
        for (auto it : llvm::enumerate(a))
        {
            auto i = it.index();
            if (i > 1)
            {
                new(&args[i]) Arg();
            }
            auto arg = it.value();
            args[i].index = static_cast<unsigned>(i);
            if (nullptr != arg)
            {
                args[i].arg = arg;
                arg->users.push_back(args[i]);
            }
        }
    }
    ~Node()
    {
        for (unsigned i = 0; i < argCount; ++i)
        {
            if (args[i].arg != nullptr)
            {
                args[i].arg->users.erase(args[i].getIterator());
            }
            if (i >= 1)
            {
                args[i].~Arg();
            }
        }
    }
    friend class MemorySSA;

    static size_t computeSize(size_t numArgs)
    {
        return sizeof(Node) + (numArgs > 1 ? sizeof(Arg) * (numArgs - 1) : 0);
    }

    mlir::Operation* operation = nullptr;
    Type type = Type::Root;
    unsigned argCount = 0;

    struct Arg : public llvm::ilist_node<Arg>
    {
        Node* arg = nullptr;
        unsigned index = 0;

        Node* getParent()
        {
            auto offset = static_cast<unsigned>(offsetof(Node, args) + sizeof(Arg) * index);
            return reinterpret_cast<Node*>(reinterpret_cast<char*>(this) - offset);
        }
    };
    llvm::simple_ilist<Arg> users;

    Arg args[1]; // Variadic size
};

plier::MemorySSA::Node* plier::MemorySSA::createNode(mlir::Operation* op, NodeType type, llvm::ArrayRef<plier::MemorySSA::Node*> args)
{
    assert(nullptr != op);
    auto ptr = allocator.Allocate(Node::computeSize(args.size()), std::alignment_of<Node>::value);
    auto node = new(ptr) Node(op, type, args);
    nodesMap[op] = node;
    nodes.push_back(*node);
    return node;
}

void plier::MemorySSA::eraseNode(plier::MemorySSA::Node* node)
{
    assert(nullptr != node);
    assert(node->getUsers().empty());
    nodes.erase(node->getIterator());
    node->~Node();
}

plier::MemorySSA::Node* plier::MemorySSA::getRoot()
{
    if (nullptr == root)
    {
        root = new(allocator.Allocate(Node::computeSize(0), std::alignment_of<Node>::value)) Node();
        nodes.push_back(*root);
    }
    return root;
}

plier::MemorySSA::Node* plier::MemorySSA::getNode(mlir::Operation* op) const
{
    assert(nullptr != op);
    auto it = nodesMap.find(op);
    return it != nodesMap.end() ? it->second : nullptr;
}

void plier::MemorySSA::print(plier::MemorySSA::Node* node, llvm::raw_ostream& os) /*const*/ // TODO: identifyObject const
{
    const llvm::StringRef types[] = {
        "MemoryRoot",
        "MemoryDef",
        "MemoryUse",
        "MemoryPhi",
    };
    auto writeId = [&](const Node* node)
    {
        if (nullptr != node)
        {
            os << *allocator.identifyObject(node);
        }
        else
        {
            os << "null";
        }
    };
    auto type = node->getType();
    writeId(node);
    os << " = ";
    os << types[static_cast<int>(type)] << "(";
    auto args = node->getArguments();
    llvm::interleaveComma(args, os, writeId);
    os << ")";
    auto users = node->getUsers();
    if (!users.empty())
    {
        os << " users: ";
        llvm::interleaveComma(users, os, writeId);
    }
    os << "\n";
}

namespace
{
plier::MemorySSA::Node* memSSAProcessRegion(mlir::Region& region, plier::MemorySSA::Node* entryNode, plier::MemorySSA& memSSA)
{
    assert(nullptr != entryNode);
    if (!llvm::hasSingleElement(region))
    {
        // Only structured control flow is supported for now
        return nullptr;
    }

    auto& block = region.front();
    plier::MemorySSA::Node* currentNode = entryNode;
    using NodeType = plier::MemorySSA::Node::Type;
    for (auto& op : block)
    {
        auto createNode = [&](NodeType type, auto args)
        {
            return memSSA.createNode(&op, type, args);
        };
        if (!op.getRegions().empty())
        {
            if (mlir::isa<mlir::scf::ForOp, mlir::scf::ParallelOp>(op))
            {
                assert(llvm::hasSingleElement(op.getRegions()));
                std::array<plier::MemorySSA::Node*, 2> phiArgs = {nullptr, currentNode};
                auto phi = createNode(NodeType::Phi, phiArgs);
                auto result = memSSAProcessRegion(op.getRegions().front(), phi, memSSA);
                if (nullptr == result)
                {
                    return nullptr;
                }

                if (result != phi)
                {
                    phi->setArgument(0, result);
                    currentNode = result;
                }
                else
                {
                    for (auto use : llvm::make_early_inc_range(phi->getUses()))
                    {
                        assert(use.user != nullptr);
                        use.user->setArgument(use.index, currentNode);
                    }
                    memSSA.eraseNode(phi);
                }
            }
            else if (mlir::isa<mlir::scf::ReduceOp>(op))
            {
                // TODO: handle reduce
            }
            else
            {
                // Unsupported op
                return nullptr;
            }
        }
        else
        {
            if (auto effects = mlir::dyn_cast<mlir::MemoryEffectOpInterface>(op))
            {
                if (effects.hasEffect<mlir::MemoryEffects::Write>())
                {
                    currentNode = createNode(NodeType::Def, currentNode);
                }
                if (effects.hasEffect<mlir::MemoryEffects::Read>())
                {
                    createNode(NodeType::Use, currentNode);
                }
            }
            else if(op.hasTrait<mlir::OpTrait::HasRecursiveSideEffects>())
            {
                currentNode = createNode(NodeType::Def, currentNode);
            }
        }

    }

    return currentNode;
}
}

llvm::Optional<plier::MemorySSA> plier::buildMemorySSA(mlir::FuncOp func)
{
    llvm::errs() << "buildMemorySSA1\n";
    plier::MemorySSA ret;
    if (nullptr == memSSAProcessRegion(func.getRegion(), ret.getRoot(), ret))
    {
        llvm::errs() << "buildMemorySSA2\n";
        return {};
    }
    llvm::errs() << "buildMemorySSA3\n";
//    ret.simplify();
    llvm::errs() << "buildMemorySSA4\n";
    func.dump();
    for (auto& node : ret.getNodes())
    {
        llvm::errs() << "\n";
        ret.print(&node, llvm::errs());
        if (node.getOperation())
        {
            node.getOperation()->dump();
        }
    }

    llvm::errs() << "buildMemorySSAend\n";
    return std::move(ret);
}
