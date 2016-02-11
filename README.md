# rota

This software makes a rota for registrars who work in the oncology department of a certain hospital.

There are many competing constraints that affect how _good_ a particular rota is, so this code uses a Monte Carlo approach:

* Implement a function that produces a numerical score for any rota, no matter how broken it is
* Start with a completely random very broken rota (people having to be in two places at once, working holidays, etc), compute its score
* Do the following around 6 million times:
	* Randomly mutate the rota using one of two strategies:
		* Reassign one shift to a different person
		* Swap people between two shifts
	* Compute the score of this mutated rota
	* Accept the mutated rota randomly or if its score is better

The process takes a few seconds on a laptop from 2013.  The software is around 1500 lines of ANSI C.

An attempt at end-user documentation can be found [here](http://sjb3d.github.io/rota/doc/).
