
- Only coerce to Bool in conditional expressions where type is not bool:
	- inject an .isTrue method, if it exists.
- Implement .isTrue method for:
	- integer
	- float
	- Option
	- Result
- Support flow typing into the block, unwrapping Option value
