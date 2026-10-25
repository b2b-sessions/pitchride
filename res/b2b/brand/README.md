# PitchRide logo and loading maze (locked)

- `logo.svg`: the logo. It is a circular maze with five rings, one gap at the bottom and one route to the centre, drawn in orange. The route ends inside a daisy petal.
- `logo-walls.svg`: the same maze without the route. Use it for the favicon and for small sizes.
- `loading.html`: the room loading animation. The line draws from the gap to the centre, then the flower blooms. The loop is 3.6 s.
- `logo-maze.json`: the fixed layout (seed 150), the route cells, the open passages, the timings and the colours. The Qt loading screen draws from this.
- `maze.py`: the generator. `python3 maze.py` prints the seed and the route. Do not change the seed: the logo depends on it.
