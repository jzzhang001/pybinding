#include "Lattice.hpp"

#include <Eigen/Dense>  // for `colPivHouseholderQr()`

#include <support/format.hpp>
using namespace fmt::literals;

namespace cpb {

Lattice::Lattice(Cartesian a1, Cartesian a2, Cartesian a3) {
    vectors.push_back(a1);
    if (!a2.isZero()) { vectors.push_back(a2); }
    if (!a3.isZero()) { vectors.push_back(a3); }
    vectors.shrink_to_fit();
}

void Lattice::add_sublattice(string_view name, Cartesian position, double onsite_energy) {
    add_sublattice(name, position, MatrixXcd::Constant(1, 1, onsite_energy).eval());
}

void Lattice::add_sublattice(string_view name, Cartesian position, VectorXd const& onsite_energy) {
    auto const size = onsite_energy.size();
    auto mat = MatrixXcd::Zero(size, size).eval();
    mat.diagonal() = onsite_energy.cast<std::complex<double>>();
    add_sublattice(name, position, mat);
}

void Lattice::add_sublattice(string_view name, Cartesian position,
                             MatrixXcd const& onsite_energy) {
    if (onsite_energy.rows() != onsite_energy.cols()) {
        throw std::logic_error("The onsite hopping term must be a real vector or a square matrix");
    }
    if (!onsite_energy.diagonal().imag().isZero()) {
        throw std::logic_error("The main diagonal of the onsite hopping term must be real");
    }
    if (!onsite_energy.isUpperTriangular() && onsite_energy != onsite_energy.adjoint()) {
        throw std::logic_error("The onsite hopping matrix must be upper triangular or Hermitian");
    }

    auto const unique_id = register_sublattice(name);
    sublattices[name] = {position, onsite_energy, unique_id, unique_id};
}

void Lattice::add_alias(string_view alias_name, string_view original_name, Cartesian position) {
    auto const& original = sublattice(original_name);
    auto const alias_id = original.unique_id;
    auto const unique_id = register_sublattice(alias_name);
    sublattices[alias_name] = {position, original.energy, unique_id, alias_id};
}

void Lattice::register_hopping_energy(std::string const& name, std::complex<double> energy) {
    register_hopping_energy(name, MatrixXcd::Constant(1, 1, energy));
}

void Lattice::register_hopping_energy(std::string const& name, MatrixXcd const& energy) {
    if (name.empty()) { throw std::logic_error("Hopping name can't be blank"); }

    constexpr auto max_size = static_cast<size_t>(std::numeric_limits<hop_id>::max());
    if (hoppings.size() > max_size) {
        throw std::logic_error("Exceeded maximum number of unique hoppings energies: "
                               + std::to_string(max_size));
    }

    auto const unique_id = static_cast<hop_id>(hoppings.size());
    auto const is_unique_name = hoppings.insert({name, {energy, unique_id, {}}}).second;
    if (!is_unique_name) { throw std::logic_error("Hopping '" + name + "' already exists"); }
}

void Lattice::add_hopping(Index3D relative_index, string_view from_sub, string_view to_sub,
                          string_view hopping_family_name) {
    if (from_sub == to_sub && relative_index == Index3D::Zero()) {
        throw std::logic_error(
            "Hoppings from/to the same sublattice must have a non-zero relative "
            "index in at least one direction. Don't define onsite energy here."
        );
    }

    auto const& from = sublattice(from_sub);
    auto const& to = sublattice(to_sub);
    auto const& hop_matrix = hopping_family(hopping_family_name).energy;

    if (from.energy.rows() != hop_matrix.rows() || to.energy.cols() != hop_matrix.cols()) {
        throw std::logic_error(
            "Hopping size mismatch: from '{}' ({}) to '{}' ({}) with matrix '{}' ({}, {})"_format(
                from_sub, from.energy.rows(), to_sub, to.energy.cols(),
                hopping_family_name, hop_matrix.rows(), hop_matrix.cols()
            )
        );
    }

    auto const dup = std::any_of(hoppings.begin(), hoppings.end(), [&](Hoppings::reference r) {
        auto const& hopping_terms = r.second.terms;
        return std::any_of(hopping_terms.begin(), hopping_terms.end(), [&](HoppingTerm const& h) {
            auto const candidate = std::tie(relative_index, from.unique_id, to.unique_id);
            auto const existing = std::tie(h.relative_index, h.from, h.to);
            auto const existing_conjugate = std::tie(-h.relative_index, h.to, h.from);
            return candidate == existing || candidate == existing_conjugate;
        });
    });
    if (dup) { throw std::logic_error("The specified hopping already exists."); }

    hoppings[hopping_family_name].terms.push_back({relative_index, from.unique_id, to.unique_id});
}

void Lattice::add_hopping(Index3D relative_index, string_view from_sub, string_view to_sub,
                          std::complex<double> energy) {
    add_hopping(relative_index, from_sub, to_sub, MatrixXcd::Constant(1, 1, energy));
}

void Lattice::add_hopping(Index3D relative_index, string_view from_sub, string_view to_sub,
                          MatrixXcd const& energy) {
    auto const hopping_name = [&] {
        // Look for an existing hopping ID with the same energy
        auto const it = std::find_if(hoppings.begin(), hoppings.end(), [&](Hoppings::reference r) {
            return r.second.energy == energy;
        });

        if (it != hoppings.end()) {
            return it->first;
        } else {
            auto const name = "__anonymous__{}"_format(hoppings.size());
            register_hopping_energy(name, energy);
            return name;
        }
    }();

    add_hopping(relative_index, from_sub, to_sub, hopping_name);
}

void Lattice::set_offset(Cartesian position) {
    if (any_of(translate_coordinates(position).array().abs() > 0.55f)) {
        throw std::logic_error("Lattice origin must not be moved by more than "
                               "half the length of a primitive lattice vector.");
    }
    offset = position;
}

Lattice::Sublattice const& Lattice::sublattice(std::string const& name) const {
    auto const it = sublattices.find(name);
    if (it == sublattices.end()) {
        throw std::out_of_range("There is no sublattice named '{}'"_format(name));
    }
    return it->second;
}

Lattice::Sublattice const& Lattice::sublattice(sub_id id) const {
    using Pair = Sublattices::value_type;
    auto const it = std::find_if(sublattices.begin(), sublattices.end(),
                                 [&](Pair const& p) { return p.second.unique_id == id; });
    if (it == sublattices.end()) {
        throw std::out_of_range("There is no sublattice with ID = {}"_format(id));
    }
    return it->second;
}

Lattice::HoppingFamily const& Lattice::hopping_family(std::string const& name) const {
    auto const it = hoppings.find(name);
    if (it == hoppings.end()) {
        throw std::out_of_range("There is no hopping named '{}'"_format(name));
    }
    return it->second;
}

Lattice::HoppingFamily const& Lattice::hopping_family(hop_id id) const {
    using Pair = Hoppings::value_type;
    auto const it = std::find_if(hoppings.begin(), hoppings.end(),
                                 [&](Pair const& p) { return p.second.unique_id == id; });
    if (it == hoppings.end()) {
        throw std::out_of_range("There is no hopping with ID = {}"_format(id));
    }
    return it->second;
}

int Lattice::max_hoppings() const {
    auto result = idx_t{0};
    for (auto const& s : optimized_structure()) {
        auto const num_scalar_hoppings = std::accumulate(
            s.hoppings.begin(), s.hoppings.end(),
            sublattice(s.alias).energy.cols() - 1, // num hops in onsite matrix (-1 for diagonal)
            [&](idx_t n, Hopping const& h) { return n + hopping_family(h.id).energy.cols(); }
        );

        result = std::max(result, num_scalar_hoppings);
    }
    return static_cast<int>(result);
}

Cartesian Lattice::calc_position(Index3D index, string_view sublattice_name) const {
    auto position = offset;
    // Bravais lattice position
    for (auto i = 0, size = ndim(); i < size; ++i) {
        position += static_cast<float>(index[i]) * vectors[i];
    }
    if (!sublattice_name.empty()) {
        position += sublattice(sublattice_name).position;
    }
    return position;
}

Vector3f Lattice::translate_coordinates(Cartesian position) const {
    auto const size = ndim();
    auto const lattice_matrix = [&]{
        auto m = Eigen::MatrixXf(size, size);
        for (auto i = 0; i < size; ++i) {
            m.col(i) = vectors[i].head(size);
        }
        return m;
    }();

    // Solve `lattice_matrix * v = p`
    auto const& p = position.head(size);
    auto v = Vector3f(0, 0, 0);
    v.head(size) = lattice_matrix.colPivHouseholderQr().solve(p);
    return v;
}

Lattice Lattice::with_offset(Cartesian position) const {
    auto new_lattice = *this;
    new_lattice.set_offset(position);
    return new_lattice;
}

Lattice Lattice::with_min_neighbors(int number) const {
    auto new_lattice = *this;
    new_lattice.min_neighbors = number;
    return new_lattice;
}

bool Lattice::has_onsite_energy() const {
    return std::any_of(sublattices.begin(), sublattices.end(), [](Sublattices::const_reference r) {
        return !r.second.energy.diagonal().isZero();
    });
}

bool Lattice::has_multiple_orbitals() const {
    return std::any_of(sublattices.begin(), sublattices.end(), [](Sublattices::const_reference r) {
        return r.second.energy.cols() != 1;
    });
}

bool Lattice::has_complex_hoppings() const {
    return std::any_of(hoppings.begin(), hoppings.end(), [](Hoppings::const_reference r) {
        return !r.second.energy.imag().isZero();
    });
}

OptimizedLatticeStructure Lattice::optimized_structure() const {
    auto opt_structure = OptimizedLatticeStructure(nsub());

    for (auto const& pair : sublattices) {
        auto const& sublattice = pair.second;
        opt_structure[sublattice.unique_id].position = sublattice.position;
        opt_structure[sublattice.unique_id].alias = sublattice.alias_id;
    }

    for (auto const& pair : hoppings) {
        auto const& hopping_family = pair.second;
        for (auto const& term : hopping_family.terms) {
            // The other sublattice has an opposite relative index (conjugate)
            opt_structure[term.from].hoppings.push_back(
                {term.relative_index, term.to, hopping_family.unique_id, /*is_conjugate*/false}
            );
            opt_structure[term.to].hoppings.push_back(
                {-term.relative_index, term.from, hopping_family.unique_id, /*is_conjugate*/true}
            );
        }
    }

    return opt_structure;
}

Lattice::NameMap Lattice::sub_name_map() const {
    auto map = NameMap();
    for (auto const& p : sublattices) {
        map[p.first] = p.second.unique_id;
    }
    return map;
}

Lattice::NameMap Lattice::hop_name_map() const {
    auto map = NameMap();
    for (auto const& p : hoppings) {
        map[p.first] = p.second.unique_id;
    }
    return map;
}

sub_id Lattice::register_sublattice(string_view name) {
    if (name.empty()) { throw std::logic_error("Sublattice name can't be blank"); }

    constexpr auto max_size = static_cast<size_t>(std::numeric_limits<sub_id>::max());
    if (sublattices.size() > max_size) {
        throw std::logic_error("Exceeded maximum number of unique sublattices: "
                               + std::to_string(max_size));
    }

    if (sublattices.find(name) != sublattices.end()) {
        throw std::logic_error("Sublattice '" + name + "' already exists");
    }

    return static_cast<sub_id>(sublattices.size());
}

} // namespace cpb
